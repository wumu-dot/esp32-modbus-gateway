/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * wifi_mqtt.c — FEAT-ESPGW-02：WiFi STA + MQTT 上行实现
 *
 * 设计要点（对齐 FEAT-ESPGW-02 改动影响文档 §4 风险对策）：
 *   - 网络凭据走 Kconfig（SSID/PASS/Broker URI 可编入 sdkconfig，不入代码）
 *   - 唯一轮询任务仍在 01 的 master_poll_task 独占主站 API（GW-04），
 *     本模块只在本任务的打印旁调用 gateway_mqtt_publish()，不另起任务抢 API
 *   - MQTT publish 用 esp_mqtt_client_publish() 异步入队、非阻塞（GW-05）
 *   - 断网/断连期轮询照常，发布跳过（§4「断网重连」对策）
 *   - JSON 直接读 &holding_regs 打包，与 01 打印同一份内存（§2 核心耦合点）
 *
 * 依赖：ESP-IDF 自带 esp-mqtt（esp_mqtt_client.h），无需第三方组件。
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>      /* FEAT-ESPGW-05：strtoul */
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "esp_netif.h"
#include "esp_timer.h"          /* 高危-3：重试耗尽后周期重连用 */
#include "sdkconfig.h"
#include "wifi_mqtt.h"

#define TAG "WIFI_MQTT"

/* WiFi 连接等待事件位 */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

/* 最大重试 / 等待：热点不可达时不无限挂起（§4 断网重连） */
#define WIFI_MAX_RETRY       (CONFIG_MB_WIFI_MAX_RETRY)
#define WIFI_CONNECT_TIMEOUT_MS (10000u)
#define WIFI_RETRY_RETRY_DELAY_MS (2000u)
#define MQTT_CONNECT_WAIT_MS (3000u)

static EventGroupHandle_t s_wifi_event_group = NULL;
/* 中危-7：ptr 在 app_main 写、轮询任务读，加 volatile 免优化无序（Xtensa 指针读写原子） */
static volatile esp_mqtt_client_handle_t s_mqtt_client = NULL;
/* 高危-3：重试预算耗尽后，用 esp_timer 周期尝试重连，避免永久离线 */
static esp_timer_handle_t s_wifi_retry_timer = NULL;
/* 残留-3：STA 已关联到 AP（等待DHCP）与否，由 STA_CONNECTED/DISCONNECTED 事件维护 */
static bool s_sta_associated = false;

/* FEAT-ESPGW-05：下行回调（NULL = 纯上行模式，不订阅） */
static gateway_cmd_cb_t s_cmd_cb = NULL;

/* ------------------------------------------------------------------ */
/* FEAT-ESPGW-05：极简 JSON 取 u16 —— 不引第三方解析库                   */
/* 只认 {"cid":N,"value":V} 形式；找不到/超范围即失败。                   */
/* buf 不保证 NUL 结尾 ⇒ 调用方必须先转存到本地缓冲。                     */
/* ------------------------------------------------------------------ */
static bool json_get_u16(const char *buf, const char *key, uint16_t *out)
{
    if (buf == NULL || key == NULL || out == NULL) {
        return false;
    }
    char pat[24];
    int pn = snprintf(pat, sizeof(pat), "\"%s\":", key);
    if (pn <= 0 || pn >= (int)sizeof(pat)) {
        return false;
    }
    const char *p = strstr(buf, pat);
    if (p == NULL) {
        return false;
    }
    p += pn;
    while (*p == ' ') {
        p++;
    }
    if (*p < '0' || *p > '9') {          /* 只接受非负十进制；负数/引号串一律拒 */
        return false;
    }
    char *end = NULL;
    unsigned long v = strtoul(p, &end, 10);
    if (end == p || v > 0xFFFFul) {
        return false;
    }
    *out = (uint16_t)v;
    return true;
}

/* 下行报文处理：解析 → 回调（⚠️ 回调在 MQTT 任务上下文，必须只入队） */
static void mqtt_handle_cmd_data(esp_mqtt_event_handle_t event)
{
    if (s_cmd_cb == NULL) {
        return;                          /* 未注册 = 纯上行模式 */
    }
    /* event->data 无 NUL 结尾 ⇒ 先转存。128 B 足够容纳本协议报文 */
    char tmp[128];
    int len = (event->data_len < (int)sizeof(tmp) - 1) ? event->data_len : (int)sizeof(tmp) - 1;
    if (len <= 0) {
        return;
    }
    memcpy(tmp, event->data, (size_t)len);
    tmp[len] = '\0';

    gateway_cmd_t cmd = { 0 };

    /* ① 旧格式（自建 / 公共 broker）：{"cid":0,"value":650} */
    if (json_get_u16(tmp, "cid", &cmd.cid) && json_get_u16(tmp, "value", &cmd.value)) {
        ESP_LOGI(TAG, "cmd recv (cid form): cid=%u value=%u",
                 (unsigned)cmd.cid, (unsigned)cmd.value);
        s_cmd_cb(&cmd);
        return;
    }

    /* ② ThingsCloud 属性下发格式：{"th":650} —— 键名 = 功能定义中的属性标识符。
     *    ⚠️ 平台可一次下发多个属性，本循环逐个映射；命令队列是覆盖式的
     *    （上游 xQueueOverwrite），多属性同时下发时只保留**最后一个**。
     *    映射表必须与 ThingsCloud「功能定义」的属性标识符逐字一致，
     *    并与从站 Modbus 寄存器 0..4 的顺序相同。 */
    static const struct { const char *key; uint16_t cid; } k_attr_map[] = {
        { "th",          0u },   /* 报警阈值       → reg 0 */
        { "alarm_cnt",   1u },   /* 连续异常帧数   → reg 1 */
        { "release_cnt", 2u },   /* 连续正常帧数   → reg 2 */
        { "win_len",     3u },   /* 滑窗长度       → reg 3（从站固定拒写） */
        { "win_anom",    4u },   /* 窗内异常阈值   → reg 4 */
    };
    bool any = false;
    for (size_t i = 0; i < (sizeof(k_attr_map) / sizeof(k_attr_map[0])); i++) {
        uint16_t v = 0;
        if (json_get_u16(tmp, k_attr_map[i].key, &v)) {
            gateway_cmd_t c = { 0 };
            c.cid = k_attr_map[i].cid;
            c.value = v;
            ESP_LOGI(TAG, "cmd recv (attr form): %s=%u -> cid=%u",
                     k_attr_map[i].key, (unsigned)v, (unsigned)c.cid);
            s_cmd_cb(&c);
            any = true;
        }
    }
    if (!any) {
        ESP_LOGW(TAG, "cmd parse fail, drop: %s", tmp);
    }
}

void gateway_mqtt_register_cmd_handler(gateway_cmd_cb_t cb)
{
    s_cmd_cb = cb;
}

void gateway_mqtt_publish_ack(uint16_t cid, uint16_t value, int err)
{
    /* ThingsCloud 没有"命令 ACK 主题"：属性上报只接受功能定义里已声明的键，
     * 擅自发 {"cid":..,"err":..} 会被平台以 406 拒收并可能断连。
     * ⇒ 回执改为【本地日志】；真正的回执由下一轮属性上报体现（改了值就能看到）。
     * 若将来改回自建 broker / 公共 broker，把下面的 publish 分支放开即可。 */
    ESP_LOGI(TAG, "ack: cid=%u value=%u err=%d (local only, ThingsCloud has no ack topic)",
             (unsigned)cid, (unsigned)value, err);
    (void)s_mqtt_client;
}

/* 周期重连回调：只需触发一次连接，失败会再由 DISCONNECTED 事件驱动后续逻辑 */
static void wifi_retry_timer_cb(void *arg)
{
    (void)arg;
    /* 残留-3：已关联到 AP（DHCP 未完成/未拿到 IP）时不再重复 esp_wifi_connect()，
     * 否则可能重新关联、打断 DHCP；等它自然拿到 IP 或断开再重连。 */
    if (s_sta_associated) {
        return;
    }
    esp_wifi_connect();
}

/* 启动周期重连（幂等：已存在则不再重复创建） */
static void wifi_start_retry_timer(void)
{
    if (s_wifi_retry_timer != NULL) {
        return;
    }
    const esp_timer_create_args_t args = {
        .callback = &wifi_retry_timer_cb,
        .name = "wifi_retry",
    };
    if (esp_timer_create(&args, &s_wifi_retry_timer) != ESP_OK) {
        ESP_LOGE(TAG, "failed to create wifi retry timer");
        return;
    }
    if (esp_timer_start_periodic(s_wifi_retry_timer,
                                 WIFI_RETRY_RETRY_DELAY_MS * 1000u) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start wifi retry timer");
    }
}

/* 连接成功即停掉周期重连，回到正常事件驱动流程 */
static void wifi_stop_retry_timer(void)
{
    if (s_wifi_retry_timer == NULL) {
        return;
    }
    esp_timer_stop(s_wifi_retry_timer);
    esp_timer_delete(s_wifi_retry_timer);
    s_wifi_retry_timer = NULL;
}

/* ------------------------------------------------------------------ */
/* WiFi 事件处理：STA 连接成功置 CONNECTED_BIT，失败计数判 FAIL_BIT     */
/* ------------------------------------------------------------------ */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    static int s_retry_num = 0;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        s_sta_associated = true;   /* 残留-3：已关联到 AP（等待DHCP），周期重连应暂停 */
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_associated = false;
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "WiFi disconnected, retry %d/%d", s_retry_num, WIFI_MAX_RETRY);
        } else {
            ESP_LOGE(TAG, "WiFi connection failed after %d retries", WIFI_MAX_RETRY);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            /* 高危-3：绝不永久停摆。预算耗尽后转入 esp_timer 周期重连，
             * 热点恢复后仍能自动连上（回调内不能长延时，故用定时器）。 */
            wifi_start_retry_timer();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        /* 残留-5：关联成功但 DHCP 一直拿不到 IP（此时无 DISCONNECTED 事件可依赖，
         * s_retry_num 不增长、周期重连本不会启动）。IP 归零时清关联标志并转周期
         * 重连自愈，避免"已关联、无 IP、不发布"的静默永久停摆。 */
        s_sta_associated = false;
        wifi_start_retry_timer();
        ESP_LOGW(TAG, "STA lost IP (DHCP failed?), entering periodic reconnect");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_retry_num = 0;
        wifi_stop_retry_timer();
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ------------------------------------------------------------------ */
/* MQTT 事件处理：连接/断开/错误打日志；断线自动重连由 esp-mqtt 处理     */
/* ------------------------------------------------------------------ */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected to broker");
        /* FEAT-ESPGW-05：只有注册了下行回调才订阅（否则保持纯上行，行为同 02） */
        if (s_cmd_cb != NULL) {
            /* ThingsCloud 只支持 QoS 0，订阅也必须用 0 */
            int mid = esp_mqtt_client_subscribe(s_mqtt_client,
                                                CONFIG_MB_MQTT_TOPIC_CMD, 0);
            if (mid < 0) {
                ESP_LOGW(TAG, "subscribe '%s' failed", CONFIG_MB_MQTT_TOPIC_CMD);
            } else {
                ESP_LOGI(TAG, "subscribed '%s' (msg_id=%d)", CONFIG_MB_MQTT_TOPIC_CMD, mid);
            }
        }
        break;
    case MQTT_EVENT_DATA: {
        /* FEAT-ESPGW-05：只处理 cmd topic 的【完整】报文。
         * ⭐ 分片判据 = current_data_offset / total_data_len
         *    （源① mqtt_client.h:193-196；源② mqtt.rst:201）。
         *    ⛔ 不是 msg_id —— 那是「MQTT messaged id of message」（mqtt_client.h:200），
         *    入站报文通常非 0，用它判分片会让所有命令被误丢。
         * ⚠️ 分片时【只有首个事件】带 topic（mqtt_client.h:69-73）⇒ 后续分片无法归属，
         *    故整条分片报文直接丢弃并告警（本项目报文 <64 B，正常不会分片）。 */
        const bool complete = (event->current_data_offset == 0) &&
                              (event->total_data_len == event->data_len);
        if (!complete) {
            ESP_LOGW(TAG, "fragmented cmd frame ignored (offset=%d total=%d len=%d)",
                     event->current_data_offset, event->total_data_len, event->data_len);
            break;
        }
        if (event->topic_len > 0) {
            const size_t tl = (size_t)event->topic_len;
            if ((tl == strlen(CONFIG_MB_MQTT_TOPIC_CMD)) &&
                (strncmp(event->topic, CONFIG_MB_MQTT_TOPIC_CMD, tl) == 0)) {
                mqtt_handle_cmd_data(event);
            }
        }
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected, code=%d", event->error_handle ?
                 event->error_handle->connect_return_code : -1);
        break;
    case MQTT_EVENT_ERROR:
        /* error_type 为 esp_mqtt_error_type_t 枚举（int），非字符串 */
        ESP_LOGE(TAG, "MQTT error, error_type=%d", event->error_handle ?
                 (int)event->error_handle->error_type : -1);
        break;
    default:
        break; /* publish ack / data / etc：不用处理 */
    }
}

/* ------------------------------------------------------------------ */
/* 初始化（public）：WiFi STA + MQTT 起动                             */
/* ------------------------------------------------------------------ */
esp_err_t wifi_mqtt_init(void)
{
    esp_err_t err = nvs_flash_init();
    /* 若上次断电致 NVS 满，擦除重试（官方惯例）。
     * 高危-2 修复：网络初始化失败只返回错误码，不再 ESP_ERROR_CHECK 复位整板，
     * 保证"失败不阻塞主站轮询"（FEAT-ESPGW-02 §4）真正成立。 */
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_erase fail 0x%x, skip wifi/mqtt", (int)err);
            return err;
        }
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init fail 0x%x, skip wifi/mqtt", (int)err);
        return err;
    }

    s_wifi_event_group = xEventGroupCreate();

    err = esp_netif_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_netif_init fail 0x%x", (int)err); return err; }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default fail 0x%x", (int)err);
        return err;
    }

    /* 注册事件（先于 wifi_init，避免丢事件）：
     * WiFi/IP 经 esp_event；MQTT 用官方 esp_mqtt_client_register_event（非 ESP_EVENT base） */
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) { ESP_LOGE(TAG, "wifi event reg fail 0x%x", (int)err); return err; }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) { ESP_LOGE(TAG, "ip event reg fail 0x%x", (int)err); return err; }
    /* 残留-5：必须显式订阅 LOST_IP。DHCP 启动即 armed lost-IP 定时器
     * （esp_netif_lwip.c:1542），定时器到点若 IP 仍为 0.0.0.0 就发此事件
     * （esp_netif_lwip.c:1418，间隔 CONFIG_ESP_NETIF_IP_LOST_TIMER_INTERVAL=120s）。
     * 少这一行，wifi_event_handler 里的 LOST_IP 分支永远收不到事件，形同死代码。 */
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                              &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) { ESP_LOGE(TAG, "ip lost event reg fail 0x%x", (int)err); return err; }

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_wifi_init fail 0x%x", (int)err); return err; }

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_MB_WIFI_SSID,
            .password = CONFIG_MB_WIFI_PASS,
            /* ⚠️ WIFI_AUTH_WPA2_WPA3_PSK（值 7）的语义是"要求 AP 处于
             * WPA2/WPA3 **混合**模式"，不是"WPA2 或 WPA3 都接受"。
             * 手机热点 / 多数家用路由是纯 WPA2_PSK（值 3）⇒ 会被拒，
             * 表现为 reason=211 (NO_AP_FOUND_IN_AUTHMODE_THRESHOLD)
             * 与 "authmode threshold failure (recvd,thresh)=(3,7)"。
             * WIFI_AUTH_WPA2_PSK 既接受纯 WPA2，也接受 WPA/WPA2 混合。 */
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_wifi_set_mode fail 0x%x", (int)err); return err; }
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_wifi_set_config fail 0x%x", (int)err); return err; }
    err = esp_wifi_start();
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_wifi_start fail 0x%x", (int)err); return err; }

    /* 等待获得 IP 或失败（带超时，避免热点缺失时永久阻塞；§4 断网重连） */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi STA connected");
    } else {
        ESP_LOGW(TAG, "WiFi STA not got IP within %ums (will still start MQTT)",
                 (unsigned)WIFI_CONNECT_TIMEOUT_MS);
        /* 不 return 错误：网关主站轮询照常，MQTT 让 esp-mqtt 重连层兜底 */
    }

    /* MQTT 客户端：Broker URI / 认证 / 保活走 Kconfig；非阻塞启动。
     * ThingsCloud 要求：username=设备 AccessToken、password=ProjectKey、
     * keepalive >= 60 s、QoS 只能 0（QoS1/2 会被平台拒绝）。 */
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_MB_MQTT_BROKER_URI,
        .credentials.username = CONFIG_MB_MQTT_USERNAME,
        .credentials.authentication.password = CONFIG_MB_MQTT_PASSWORD,
        .session.keepalive = 60,
    };
    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "MQTT client init failed");
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    err = esp_mqtt_client_start(s_mqtt_client);
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_mqtt_client_start fail 0x%x", (int)err); return err; }

    /* 给 esp-mqtt 一点时间尝试连 broker（不阻塞轮询任务决策） */
    vTaskDelay(pdMS_TO_TICKS(MQTT_CONNECT_WAIT_MS));
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 发布（public）：JSON 打包 holding_regs 并异步 publish               */
/* 只读入参指针，不复制字段（与 01 打印同一份内存，§2 核心耦合点）。    */
/* 断线（MQTT 未连接）时跳过发布（§4 断网重连对策）。                  */
/* ------------------------------------------------------------------ */
void gateway_mqtt_publish(const sounddog_holding_t *regs, uint16_t ok_count)
{
    if (regs == NULL) {
        return;
    }
    /* MQTT 未 init 时直接跳过（轮询任务仍在打印，双出解耦） */
    if (s_mqtt_client == NULL) {
        return;
    }
    /* 断线时跳过：v5.4 esp-mqtt 无公开 is_connected/is_get_connected API，
     * 已依赖 CONFIG_MQTT_SKIP_PUBLISH_IF_DISCONNECTED（sdkconfig.defaults 使能）
     * 由发布层在未连接时自动丢弃，不入队堆积（GW-05 周期不受影响）。 */

    /* 9×Holding → JSON（字段名与 mb_param.h 语义一致）。
     * P-4 残留-1：附 ok 成功读取数；上位机据此判断数据新鲜度（ok<9=部分成功混合快照）。
     * 5..8 为只读遥测：温度/湿度在【这里】做偏移与缩放换算，云端直接可读
     *   （从站 reg5 = 温度×10 + 500 ⇒ 753 → 25.3℃；reg6 = 湿度×10 ⇒ 612 → 61.2）。
     * ⚠️ 新增的 4 个键必须在 ThingsCloud「功能定义」里先建好同名属性，
     *    否则平台按未定义键拒收（406），甚至断开连接。 */
    char payload[256];
    int n = snprintf(payload, sizeof(payload),
                     "{\"th\":%u,\"alarm_cnt\":%u,\"release_cnt\":%u,"
                     "\"win_len\":%u,\"win_anom\":%u,"
                     "\"temp\":%.1f,\"humi\":%.1f,"
                     "\"vib_rms\":%u,\"vib_peak\":%u,\"ok\":%u}",
                     (unsigned)regs->th, (unsigned)regs->alarm_cnt,
                     (unsigned)regs->release_cnt, (unsigned)regs->win_len,
                     (unsigned)regs->win_anom,
                     ((double)(int32_t)regs->temp - SD_TEMP_OFFSET) / SD_TEMP_SCALE,
                     (double)regs->humi / SD_HUMI_SCALE,
                     (unsigned)regs->vib_rms, (unsigned)regs->vib_peak,
                     (unsigned)ok_count);
    if (n < 0 || n >= (int)sizeof(payload)) {
        ESP_LOGE(TAG, "payload too long or snprintf error");
        return;
    }

    /* 异步入队后立即返回；msg_id<0 表示入队失败 */
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, CONFIG_MB_MQTT_TOPIC,
                                         payload, (int)strlen(payload), 0, 0);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "MQTT publish enqueue failed");
    }
}