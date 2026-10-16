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
#include "esp_log.h"
#include "sdkconfig.h"
#include "mbcontroller.h"
#include "esp_modbus_master.h"
#include "mb_param.h"
#include "wifi_mqtt.h"          /* FEAT-ESPGW-02：MQTT 上行（与打印双出一源） */

#define TAG "MB_MASTER"

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
const mb_parameter_descriptor_t device_parameters[MASTER_PARAM_CNT] = {
    // { CID, ParamName, Units, SlaveAddr, RegType, RegStart, RegSize, InstanceOffset, DataType, DataSize, Options, Access }
    { 0, STR("TH"),        STR("x100"), SD_DEVICE_ADDR, MB_PARAM_HOLDING,
      HOLD_REG_START(th),        HOLD_REG_SIZE(th),
      HOLD_OFFSET(th),           PARAM_TYPE_U16, PARAM_SIZE_U16,
      OPTS( SD_PARAM_TH_MIN_x100,  SD_PARAM_TH_MAX_x100, 0 ), PAR_PERMS_READ_WRITE },
    { 1, STR("ALARM_CNT"), STR("frames"), SD_DEVICE_ADDR, MB_PARAM_HOLDING,
      HOLD_REG_START(alarm_cnt),   HOLD_REG_SIZE(alarm_cnt),
      HOLD_OFFSET(alarm_cnt),      PARAM_TYPE_U16, PARAM_SIZE_U16,
      OPTS( SD_PARAM_FRAME_MIN,     SD_PARAM_FRAME_MAX, 0 ),   PAR_PERMS_READ_WRITE },
    { 2, STR("RELEASE_CNT"), STR("frames"), SD_DEVICE_ADDR, MB_PARAM_HOLDING,
      HOLD_REG_START(release_cnt), HOLD_REG_SIZE(release_cnt),
      HOLD_OFFSET(release_cnt),    PARAM_TYPE_U16, PARAM_SIZE_U16,
      OPTS( SD_PARAM_FRAME_MIN,     SD_PARAM_FRAME_MAX, 0 ),   PAR_PERMS_READ_WRITE },
    { 3, STR("WIN_LEN"),   STR("frames"), SD_DEVICE_ADDR, MB_PARAM_HOLDING,
      HOLD_REG_START(win_len),     HOLD_REG_SIZE(win_len),
      HOLD_OFFSET(win_len),        PARAM_TYPE_U16, PARAM_SIZE_U16,
      OPTS( SD_PARAM_WIN_LEN_FIX,   SD_PARAM_WIN_LEN_FIX, 0 ), PAR_PERMS_READ_WRITE },
    { 4, STR("WIN_ANOM"),  STR("frames"), SD_DEVICE_ADDR, MB_PARAM_HOLDING,
      HOLD_REG_START(win_anom),    HOLD_REG_SIZE(win_anom),
      HOLD_OFFSET(win_anom),       PARAM_TYPE_U16, PARAM_SIZE_U16,
      OPTS( SD_PARAM_FRAME_MIN,     SD_PARAM_FRAME_MAX, 0 ),   PAR_PERMS_READ_WRITE },
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
/* 轮询任务：唯一任务独占主站 API（GW-04），周期读取 5×Holding 并打印  */
/* ------------------------------------------------------------------ */

static void master_poll_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Start SoundDog master polling, %u params, period %ums",
             (unsigned)MASTER_PARAM_CNT, (unsigned)POLL_PERIOD_MS);

    for (;;) {
        /* P-4：统计本轮成功读取份数，全失败时不得把零初始化/陈旧值当真实数据上行 */
        uint16_t round_ok = 0;
        for (uint16_t cid = 0; cid < MASTER_PARAM_CNT; cid++) {
            void *inst = master_get_param_data(&device_parameters[cid]);
            uint8_t type = 0;
            esp_err_t err = mbc_master_get_parameter(master_handle, cid, (uint8_t *)inst, &type);
            if (err != ESP_OK) {
                /* GW-02：0x106 越界/不支持 ≠ 链路断；GW-10：0x107 超时先查链路/波特率 */
                ESP_LOGW(TAG, "CID#%u %s read fail, err=0x%x (%s).",
                         (unsigned)cid, (char*)device_parameters[cid].param_key,
                         (int)err, esp_err_to_name(err));
                continue;
            }
            round_ok++;
            /* 单寄存器 u16 读成功即打印该字段 */
            uint16_t val = *((uint16_t *)inst);
            ESP_LOGI(TAG, "CID#%u %s = %u", (unsigned)cid,
                     (char *)device_parameters[cid].param_key, (unsigned)val);
        }
        /* FEAT-ESPGW-02：整轮读完，把同一份 holding_regs 快照发布（打印/MQTT 双出一源）。
         * P-4：本轮至少 1 次成功才发布；首次成功前（含从站一直离线）不发布，
         * 避免把全零初始值或陈旧值当作真实数据推给上位机。
         * 残留-1：payload 附 ok=本轮成功数，部分成功时上位机可知是混合快照。 */
        if (round_ok > 0) {
            if (round_ok < MASTER_PARAM_CNT) {
                /* 残留-4：部分失败不再静默，明确告警 */
                ESP_LOGW(TAG, "round partial read: %u/%u ok, publishing mixed snapshot",
                         (unsigned)round_ok, (unsigned)MASTER_PARAM_CNT);
            }
            gateway_mqtt_publish(&holding_regs, round_ok);
        } else {
            ESP_LOGW(TAG, "round had 0 ok reads, MQTT publish skipped (slave offline?)");
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

    err = mbc_master_set_descriptor(master_handle, &device_parameters[0], MASTER_PARAM_CNT);
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

    /* 残留-2：主站 init 失败不复位整板（UART 冲突/硬件故障时 abort 会陷入
     * 复位-再失败-再复位循环）。打印错误并直接返回，不再启动轮询与上行。 */
    esp_err_t mErr = master_init();
    if (mErr != ESP_OK) {
        ESP_LOGE(TAG, "master init fail 0x%x (%s), aborting startup (no reset).",
                 (int)mErr, esp_err_to_name(mErr));
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 轮询任务：最先启动，唯一任务访问主站 API（GW-04）。
     * 置先于网络 init，保证轮询不受 WiFi/MQTT 启动时延影响（FEAT-ESPGW-02 §4）。 */
    xTaskCreatePinnedToCore(master_poll_task, "modbus_poll", 6144, NULL, 1, NULL, 1);

    /* FEAT-ESPGW-02：WiFi + MQTT 上行独立启动，失败不阻塞主站轮询 */
    esp_err_t wErr = wifi_mqtt_init();
    if (wErr != ESP_OK) {
        ESP_LOGW(TAG, "WiFi/MQTT init warn, err=0x%x (%s); continue polling.",
                 (int)wErr, esp_err_to_name(wErr));
    }
}