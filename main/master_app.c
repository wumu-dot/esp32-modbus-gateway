/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * master_app.c — ESP32 Modbus RTU 主站：初始化 + 数据字典 + 周期轮询 + 串口打印
 *
 * 基准：Espressif 官方 esp-modbus（v2.x）examples/serial/mb_serial_master/main/serial_master.c
 *   （.reference/esp-modbus/examples/serial/mb_serial_master/）
 * 裁剪决策详见 FEAT-ESPGW-01 §2（C-01..C-07）：
 *   - C-01/C-02 删自定义命令(0x41)与 Report Slave ID(0x11)
 *   - C-03 删 EXT_TYPE_SUPPORT 扩展类型（字典仅 u16 Holding）
 *   - C-04 删 Alarm/relay 业务 → 纯轮询+打印
 *   - C-05 轮询周期 改 1000ms
 *   - C-07 字典 换 SoundDog 5×Holding（mb_param.h）
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"     /* FEAT-ESPGW-05：下行命令队列 */
#include "esp_log.h"
#include "sdkconfig.h"
#include "mbcontroller.h"
#include "esp_modbus_master.h"
#include "mb_param.h"
#include "driver/uart.h"        /* FEAT-ESPGW-04：uart_flush_input（清 RX FIFO，对位官方 #16 建议②） */
#include "wifi_mqtt.h"          /* FEAT-ESPGW-02：MQTT 上行（与打印双出一源） */

#define TAG "MB_MASTER"

/* FEAT-ESPGW-04：
 *  - 数据字典由「5 个 CID 各读 1 寄存器」改为「1 个 CID 一次读 5 寄存器」
 *  - 读失败/读到不合理值 => 清 RX FIFO 后重读一次（官方 #16 建议②③）
 *  - 轮询任务栈 6144 -> 8192（候选 A：栈溢出写坏堆的诊断性加固）
 */

/* UART 端口号/引脚/波特率来自 Kconfig.projbuild */
#define MB_PORT_NUM     (CONFIG_MB_UART_PORT_NUM)
#define MB_DEV_SPEED    (CONFIG_MB_UART_BAUD_RATE)

/* 轮询周期：1000ms（预研 §7.3 项3 已定；关联 GW-05） */
#define POLL_PERIOD_MS        (1000u)
#define POLL_PERIOD_TICKS     (pdMS_TO_TICKS(POLL_PERIOD_MS))

/* Modbus 主站控制器句柄（v2.x 多实例，所有 API 传 handle） */
static void *master_handle = NULL;

/* 数据字典实例：5×Holding 快照缓冲（字段顺序 = 帧地址，见 mb_param.h） */
sounddog_holding_t holding_regs;

/* ------------------------------------------------------------------ */
/* 数据字典表（SoundDog 5×Holding，0 基址）定义                        */
/* 0 基址确认：HOLD_REG_START(field)=offsetof>>1，TH=0x0000…ANOM=0x0004 */
/* 禁止 +1（R34/GW-01）。                                              */
/* ------------------------------------------------------------------ */
/*
 * FEAT-ESPGW-05 三源预研（R27，证据见 docs/features/FEAT-ESPGW-05 §1）：
 *   source@① 源码：managed_components/espressif__esp-modbus/.../mbc_serial_master.c:134-153
 *            （set_descriptor 只校验 cid==序号/param_key/mb_size>0 ⇒ 读整块与写单条可共存）
 *            mbc_serial_master.c:435-470（写路径 param_size 语义：单寄存器给 2）
 *            esp_modbus_master.h:400（mbc_master_set_parameter 签名）
 *   Issue@② https://github.com/espressif/esp-modbus/issues/164（迟到帧 use-after-free，仍未修）
 *            https://github.com/espressif/esp-modbus/issues/16（RTU 无事务号 ⇒ 读必须整块）
 *   坑清单@③ docs/features/FEAT-ESPGW-05 §1「源③」K1~K8（含本次实踩 K6/K7）
 */
/*
 * FEAT-ESPGW-04 / 05：字典 = 1 条整块读 + 4 条单寄存器写
 *
 * 读（CID 0）
 *   RegStart      = HOLD_REG_START(th)      = 0x0000
 *   RegSize       = MASTER_PARAM_CNT        = 5（寄存器个数）
 *   InstanceOffset= HOLD_OFFSET(th)         = 1  ⇒ 指向 &holding_regs.th（= 结构体首字节）
 *   DataSize      = SD_HOLD_ALL_BYTES       = 10（读路径以 param_size 逐 u16 拷入，
 *                                              必须给字节数）
 *
 * 写（CID 1~4，FEAT-ESPGW-05）
 *   RegSize = 1，DataSize = PARAM_SIZE_U16 = 2
 *   ⚠️ 写路径的语义与读相反：set_parameter 先 calloc(mb_size<<1) 再把
 *      【param_size 字节】从实例拷进请求缓冲 ⇒ 单寄存器写必须给 2，⛔ 不是 10。
 *   ⚠️ 不能整块写 5 个：从站整批写是原子的（任一越界 ⇒ 全批拒），
 *      且 WIN_LEN 固定拒写 ⇒ 整块写必定全批失败。
 *   ⚠️ 寄存器 3（WIN_LEN）刻意【没有】写 CID。
 */
const mb_parameter_descriptor_t device_parameters[MASTER_CID_CNT] = {
    // { CID, ParamName, Units, SlaveAddr, RegType, RegStart, RegSize, InstanceOffset, DataType, DataSize, Options, Access }
    /* ---- 读：一次读满 5 个保持寄存器 ---- */
    { CID_READ_ALL, STR("SD_HOLD_ALL"), STR("-"), SD_DEVICE_ADDR, MB_PARAM_HOLDING,
      HOLD_REG_START(th),        MASTER_PARAM_CNT,
      HOLD_OFFSET(th),           PARAM_TYPE_U16, SD_HOLD_ALL_BYTES,
      OPTS( 0, 0, 0 ), PAR_PERMS_READ },

    /* ---- 写：逐字段，各 1 个寄存器 ---- */
    { CID_WR_TH, STR("TH"), STR("x100"), SD_DEVICE_ADDR, MB_PARAM_HOLDING,
      HOLD_REG_START(th),        HOLD_REG_SIZE(th),
      HOLD_OFFSET(th),           PARAM_TYPE_U16, PARAM_SIZE_U16,
      OPTS( SD_PARAM_TH_MIN_x100, SD_PARAM_TH_MAX_x100, 0 ), PAR_PERMS_READ_WRITE },

    { CID_WR_ALARM_CNT, STR("ALARM_CNT"), STR("frames"), SD_DEVICE_ADDR, MB_PARAM_HOLDING,
      HOLD_REG_START(alarm_cnt), HOLD_REG_SIZE(alarm_cnt),
      HOLD_OFFSET(alarm_cnt),    PARAM_TYPE_U16, PARAM_SIZE_U16,
      OPTS( SD_PARAM_FRAME_MIN,   SD_PARAM_FRAME_MAX, 0 ), PAR_PERMS_READ_WRITE },

    { CID_WR_RELEASE_CNT, STR("RELEASE_CNT"), STR("frames"), SD_DEVICE_ADDR, MB_PARAM_HOLDING,
      HOLD_REG_START(release_cnt), HOLD_REG_SIZE(release_cnt),
      HOLD_OFFSET(release_cnt),  PARAM_TYPE_U16, PARAM_SIZE_U16,
      OPTS( SD_PARAM_FRAME_MIN,   SD_PARAM_FRAME_MAX, 0 ), PAR_PERMS_READ_WRITE },

    { CID_WR_WIN_ANOM, STR("WIN_ANOM"), STR("frames"), SD_DEVICE_ADDR, MB_PARAM_HOLDING,
      HOLD_REG_START(win_anom),  HOLD_REG_SIZE(win_anom),
      HOLD_OFFSET(win_anom),     PARAM_TYPE_U16, PARAM_SIZE_U16,
      OPTS( SD_PARAM_FRAME_MIN,   SD_PARAM_FRAME_MAX, 0 ), PAR_PERMS_READ_WRITE },
};

/* FEAT-ESPGW-05：寄存器号 → 写 CID（索引 = 寄存器号；CID_WR_NONE = 不可写） */
static const uint16_t s_reg_to_wr_cid[MASTER_PARAM_CNT] = {
    CID_WR_TH, CID_WR_ALARM_CNT, CID_WR_RELEASE_CNT, CID_WR_NONE, CID_WR_WIN_ANOM
};

/* FEAT-ESPGW-05：下行命令队列（单深度，新命令覆盖旧命令）
 * ⚠️ GW-04：MQTT 回调【只入队】，真正的 set_parameter 由 master_poll_task 执行。 */
static QueueHandle_t s_cmd_queue = NULL;

/* FEAT-ESPGW-04：整块快照的合理性校验（官方 #16 建议③：应用层自校验）
 * 依据 SoundDog 从站自身语义（tools/modbus_slave.py）：
 *   TH        ∈ [100, 2000]（×100 定点）
 *   ALARM_CNT / RELEASE_CNT / WIN_ANOM ∈ [1, 100]
 *   WIN_LEN   ≡ 100（从站固定、拒写）
 * ⇒ 整块读只要有一项不符，就说明这一帧不是「对本次请求的合法应答」
 *   （回声帧的 byte count = 0 ⇒ 全 0；迟到帧 ⇒ 值整体错位）⇒ 丢弃重读。
 */
static bool master_snapshot_plausible(const sounddog_holding_t *p)
{
    if (!p) {
        return false;
    }
    if (p->win_len != SD_PARAM_WIN_LEN_FIX) {
        return false;
    }
    if ((p->th < SD_PARAM_TH_MIN_x100) || (p->th > SD_PARAM_TH_MAX_x100)) {
        return false;
    }
    if ((p->alarm_cnt < SD_PARAM_FRAME_MIN) || (p->alarm_cnt > SD_PARAM_FRAME_MAX)) {
        return false;
    }
    if ((p->release_cnt < SD_PARAM_FRAME_MIN) || (p->release_cnt > SD_PARAM_FRAME_MAX)) {
        return false;
    }
    if ((p->win_anom < SD_PARAM_FRAME_MIN) || (p->win_anom > SD_PARAM_FRAME_MAX)) {
        return false;
    }
    return true;
}

/* 打印字段名（保持原有 CID#n NAME 的可读格式，便于与历史日志对照） */
static const char *const s_field_name[MASTER_PARAM_CNT] = {
    "TH", "ALARM_CNT", "RELEASE_CNT", "WIN_LEN", "WIN_ANOM"
};

/* 从数据字典取字段实例指针（官方同款：param_offset 指向结构体字段） */
static void *master_get_param_data(const mb_parameter_descriptor_t *param_descriptor)
{
    assert(param_descriptor != NULL);
    void *instance_ptr = NULL;
    if (param_descriptor->param_offset != 0) {
        switch (param_descriptor->mb_param_type) {
        case MB_PARAM_HOLDING:
            /* 中危-9：void* 指针算术是 GCC 扩展，改 (uint8_t*) 严格 C 合规 */
            instance_ptr = ((uint8_t *)&holding_regs + param_descriptor->param_offset - 1);
            break;
        default:
            instance_ptr = NULL;
            break;
        }
    } else {
        ESP_LOGE(TAG, "Wrong parameter offset for CID #%u", (unsigned)param_descriptor->cid);
        assert(instance_ptr != NULL);
    }
    return instance_ptr;
}

/* ------------------------------------------------------------------ */
/* FEAT-ESPGW-05：下行写执行（⚠️ 只在 master_poll_task 里调用，守 GW-04） */
/* ------------------------------------------------------------------ */
static void master_apply_cmd(const gateway_cmd_t *cmd)
{
    if (cmd->cid >= MASTER_PARAM_CNT) {
        ESP_LOGW(TAG, "cmd rejected: cid=%u out of range (0..%u)",
                 (unsigned)cmd->cid, (unsigned)(MASTER_PARAM_CNT - 1));
        gateway_mqtt_publish_ack(cmd->cid, cmd->value, ESP_ERR_INVALID_ARG);
        return;
    }
    const uint16_t wr_cid = s_reg_to_wr_cid[cmd->cid];
    if (wr_cid == CID_WR_NONE) {
        /* WIN_LEN 从站固定 100 并拒写 ⇒ 本地先拒，省一次注定失败的往返 */
        ESP_LOGW(TAG, "cmd rejected: reg %u (WIN_LEN) is read-only on slave",
                 (unsigned)cmd->cid);
        gateway_mqtt_publish_ack(cmd->cid, cmd->value, ESP_ERR_NOT_SUPPORTED);
        return;
    }

    uint16_t val = cmd->value;
    uint8_t type = 0;
    esp_err_t err = mbc_master_set_parameter(master_handle, wr_cid,
                                             (uint8_t *)&val, &type);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "cmd applied: reg %u (%s) = %u",
                 (unsigned)cmd->cid, s_field_name[cmd->cid], (unsigned)val);
    } else {
        /* GW-02：0x106 越界/不支持 ≠ 链路断；GW-10：0x107 超时先查链路/波特率 */
        ESP_LOGW(TAG, "cmd write fail: reg %u (%s) = %u, err=0x%x (%s)",
                 (unsigned)cmd->cid, s_field_name[cmd->cid], (unsigned)val,
                 (int)err, esp_err_to_name(err));
    }
    gateway_mqtt_publish_ack(cmd->cid, val, (int)err);
}

/* MQTT 下行回调（⚠️ 在 MQTT 任务上下文！只入队，⛔ 不碰主站 API） */
static void master_on_mqtt_cmd(const gateway_cmd_t *cmd)
{
    if ((s_cmd_queue == NULL) || (cmd == NULL)) {
        return;
    }
    /* 单深度覆盖写：同一时刻只保留最新一条命令，避免积压 */
    if (xQueueOverwrite(s_cmd_queue, cmd) != pdTRUE) {
        ESP_LOGW(TAG, "cmd queue overwrite failed");
    }
}

/* ------------------------------------------------------------------ */
/* 轮询任务：唯一任务独占主站 API（GW-04），周期读取 5×Holding 并打印  */
/* ------------------------------------------------------------------ */

static void master_poll_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Start SoundDog master polling, %u reg(s) via %u CID, period %ums",
             (unsigned)MASTER_PARAM_CNT, (unsigned)MASTER_CID_CNT, (unsigned)POLL_PERIOD_MS);

    for (;;) {
        /* FEAT-ESPGW-04：一轮 = 【一次】请求（读满 5 个保持寄存器）+ 最多一次重试。
         * 官方 #16：读失败后等一个响应超时、清 RX FIFO、再重发一次。 */
        bool round_ok = false;
        uint8_t type = 0;

        for (int attempt = 0; (attempt < 2) && !round_ok; attempt++) {
            if (attempt > 0) {
                /* 清 RX FIFO：丢掉可能残留的回声帧 / 上一事务的迟到应答 */
                (void)uart_flush_input(MB_PORT_NUM);
            }
            void *inst = master_get_param_data(&device_parameters[0]);
            esp_err_t err = mbc_master_get_parameter(master_handle, 0, (uint8_t *)inst, &type);
            if (err != ESP_OK) {
                /* GW-02：0x106 越界/不支持 ≠ 链路断；GW-10：0x107 超时先查链路/波特率 */
                ESP_LOGW(TAG, "block read fail (attempt %d), err=0x%x (%s).",
                         attempt + 1, (int)err, esp_err_to_name(err));
                continue;
            }
            if (!master_snapshot_plausible(&holding_regs)) {
                /* 官方 #16 建议③：应用层自校验。整块读里只要一项越界，
                 * 说明这一帧不是对本次请求的合法应答 ⇒ 丢弃重读，不上行。 */
                ESP_LOGW(TAG, "block read implausible (attempt %d), discard+retry: "
                         "TH=%u ALARM_CNT=%u RELEASE_CNT=%u WIN_LEN=%u WIN_ANOM=%u",
                         attempt + 1,
                         (unsigned)holding_regs.th, (unsigned)holding_regs.alarm_cnt,
                         (unsigned)holding_regs.release_cnt, (unsigned)holding_regs.win_len,
                         (unsigned)holding_regs.win_anom);
                continue;
            }
            round_ok = true;
        }

        /* P-4：只有整块校验通过才打印/上行；全失败时不得把零初始化或陈旧值当真实数据 */
        if (round_ok) {
            const uint16_t *f = (const uint16_t *)&holding_regs;
            for (uint16_t i = 0; i < MASTER_PARAM_CNT; i++) {
                ESP_LOGI(TAG, "CID#%u %s = %u", (unsigned)i, s_field_name[i], (unsigned)f[i]);
            }
            gateway_mqtt_publish(&holding_regs, MASTER_PARAM_CNT);
        } else {
            ESP_LOGW(TAG, "round had 0 ok reads, MQTT publish skipped (slave offline?)");
        }

        /* FEAT-ESPGW-05：本轮读事务已结束 ⇒ 执行最多一条下行写命令。
         * ⚠️ 放在读之后，避免打断正在进行的读事务；非阻塞取（0 tick）。 */
        gateway_cmd_t cmd;
        if ((s_cmd_queue != NULL) &&
            (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE)) {
            master_apply_cmd(&cmd);
        }

        vTaskDelay(POLL_PERIOD_TICKS);
    }
}

/* ------------------------------------------------------------------ */
/* 主站初始化（官方 master_init 骨架，去掉自定义命令/Report Slave ID）  */
/* ------------------------------------------------------------------ */
static esp_err_t master_init(void)
{
    mb_communication_info_t comm = {
        .ser_opts.port = MB_PORT_NUM,
#if CONFIG_MB_COMM_MODE_ASCII
        .ser_opts.mode = MB_ASCII,
#elif CONFIG_MB_COMM_MODE_RTU
        .ser_opts.mode = MB_RTU,
#endif
        .ser_opts.baudrate = MB_DEV_SPEED,
        .ser_opts.parity = MB_PARITY_NONE,          /* 8N1 无校验（GW-10） */
        .ser_opts.uid = 0,                          /* 主站自身地址置 0 */
        .ser_opts.response_tout_ms = 1000,
        .ser_opts.data_bits = UART_DATA_8_BITS,
        .ser_opts.stop_bits = UART_STOP_BITS_1
    };

    esp_err_t err = mbc_master_create_serial(&comm, &master_handle);
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE, TAG,
                       "mb master create fail, returns(0x%x).", (int)err);
    MB_RETURN_ON_FALSE((master_handle != NULL), ESP_ERR_INVALID_STATE, TAG,
                       "mb controller initialization fail.");

    /* 绑定 UART 引脚（RXD/TXD/RTS=DE；GW-07 RTS 接 MAX3485 DE+）/RE） */
    err = uart_set_pin(MB_PORT_NUM, CONFIG_MB_UART_TXD, CONFIG_MB_UART_RXD,
                       CONFIG_MB_UART_RTS, UART_PIN_NO_CHANGE);
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE, TAG,
                       "mb serial set pin failure, uart_set_pin() returned (0x%x).", (int)err);

    /* FEAT-ESPGW-04：描述符个数 = MASTER_CID_CNT（1，整块读），不再是 MASTER_PARAM_CNT */
    err = mbc_master_set_descriptor(master_handle, &device_parameters[0], MASTER_CID_CNT);
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE, TAG,
                       "mb controller set descriptor fail, returns(0x%x).", (int)err);

    /* RS485 半双工：RTS 自动驱动 DE/RE（GW-07） */
    err = uart_set_mode(MB_PORT_NUM, UART_MODE_RS485_HALF_DUPLEX);
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE, TAG,
                       "mb serial set mode failure, uart_set_mode() returned (0x%x).", (int)err);

    vTaskDelay(pdMS_TO_TICKS(5));

    err = mbc_master_start(master_handle);
    MB_RETURN_ON_FALSE((err == ESP_OK), ESP_ERR_INVALID_STATE, TAG,
                       "mb controller start fail, returned (0x%x).", (int)err);

    ESP_LOGI(TAG, "Modbus master stack initialized. Port=%d, %u baud 8N1, RTS=GPIO%d",
             (int)MB_PORT_NUM, (int)MB_DEV_SPEED, (int)CONFIG_MB_UART_RTS);
    return err;
}

void app_main(void)
{
    /* 组件级日志：Modbus 协议链路可观测（官方同款） */
    esp_log_level_set("mb_object.master", ESP_LOG_DEBUG);
    esp_log_level_set("mbc_serial.master", ESP_LOG_DEBUG);
    /* FEAT-ESPGW-04（级 4：坐实"回声帧"）：
     * mb_master.c 的 MB_PRT_BUF(:MB_RECV / :MB_TRANSMIT) 与 port_serial.c 的
     * :PORT_SEND 用的是【动态 tag】= parent_name + ":MB_RECV"（mb_common.h:71），
     * 上面两行固定 tag 的 DEBUG 覆盖不到它们 ⇒ 当前默认级别 INFO 把它们全滤掉了
     * （这正是历史抓包里 :MB_RECV / :MB_TRANSMIT / :PORT_SEND 都是 0 次的原因）。
     * esp_log_level_set("*", DEBUG) 改的是"默认级别"，动态 tag 才会打印。
     * ⚠️ 诊断期专用：日志量明显上升，抓到第一现场后须关掉（见 FEAT-ESPGW-04 §回滚）。 */
    esp_log_level_set("*", ESP_LOG_DEBUG);

    /* 残留-2：主站 init 失败不复位整板（UART 冲突/硬件故障时 abort 会陷入
     * 复位-再失败-再复位循环）。打印错误并直接返回，不再启动轮询与上行。 */
    esp_err_t mErr = master_init();
    if (mErr != ESP_OK) {
        ESP_LOGE(TAG, "master init fail 0x%x (%s), aborting startup (no reset).",
                 (int)mErr, esp_err_to_name(mErr));
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    /* FEAT-ESPGW-05：先建下行队列 + 注册回调，再起轮询任务与 WiFi/MQTT。
     * ⚠️ 顺序有意义：回调注册必须先于 wifi_mqtt_init()，否则 MQTT_EVENT_CONNECTED
     *    到达时 s_cmd_cb 仍为 NULL ⇒ 不会订阅 cmd topic（退化成纯上行）。 */
    s_cmd_queue = xQueueCreate(1, sizeof(gateway_cmd_t));
    if (s_cmd_queue == NULL) {
        ESP_LOGE(TAG, "cmd queue create fail, downlink control disabled");
    } else {
        gateway_mqtt_register_cmd_handler(master_on_mqtt_cmd);
    }

    /* 轮询任务：最先启动，唯一任务访问主站 API（GW-04）。
     * 置先于网络 init，保证轮询不受 WiFi/MQTT 启动时延影响（FEAT-ESPGW-02 §4）。 */
    /* FEAT-ESPGW-04：栈 6144 -> 8192（候选 A：栈溢出写坏堆的诊断性加固；
     * 本轮同时把日志级别提到 DEBUG，esp_log 格式化开销上升，栈必须留够） */
    xTaskCreatePinnedToCore(master_poll_task, "modbus_poll", 8192, NULL, 1, NULL, 1);

    /* FEAT-ESPGW-02：WiFi + MQTT 上行独立启动，失败不阻塞主站轮询 */
    esp_err_t wErr = wifi_mqtt_init();
    if (wErr != ESP_OK) {
        ESP_LOGW(TAG, "WiFi/MQTT init warn, err=0x%x (%s); continue polling.",
                 (int)wErr, esp_err_to_name(wErr));
    }
}