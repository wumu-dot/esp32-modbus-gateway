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
        break;
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
            .threshold.authmode = WIFI_AUTH_WPA2_WPA3_PSK,  /* 中危-8：兼容 WPA3，不再只认 WPA2 */
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

    /* MQTT 客户端：Broker URI 走 Kconfig；非阻塞启动 */
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_MB_MQTT_BROKER_URI,
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

    /* 5×Holding → JSON（字段名与 mb_param.h 语义一致）。
     * P-4 残留-1：附 ok 成功读取数；上位机据此判断数据新鲜度（ok<5=部分成功混合快照）。 */
    char payload[192];
    int n = snprintf(payload, sizeof(payload),
                     "{\"th\":%u,\"alarm_cnt\":%u,\"release_cnt\":%u,"
                     "\"win_len\":%u,\"win_anom\":%u,\"ok\":%u}",
                     (unsigned)regs->th, (unsigned)regs->alarm_cnt,
                     (unsigned)regs->release_cnt, (unsigned)regs->win_len,
                     (unsigned)regs->win_anom, (unsigned)ok_count);
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