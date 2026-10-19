/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * wifi_mqtt.h — FEAT-ESPGW-02：WiFi STA + MQTT 上行接口
 *
 * 在 FEAT-ESPGW-01（Modbus 轮询 + 串口打印）之上叠加 MQTT 上行通道：
 * 把主站轮询到的 SoundDog 5×Holding 快照，经 WiFi 连热点、以 JSON 发布到
 * PC 本地 Broker。与 01 的串口打印**双出**、同一数据源。
 *
 * 依赖：ESP-IDF 自带 esp-mqtt（mqtt_client.h），无第三方；
 * 网络凭据（SSID/PASS/Broker URI）全部来自 Kconfig（sdkconfig 可配，不入代码）。
 */

#ifndef WIFI_MQTT_H
#define WIFI_MQTT_H

#include "esp_err.h"
#include "mb_param.h"

/**
 * @brief 初始化 WiFi（STA 连热点）+ MQTT 客户端并启动。
 *
 * 阻塞等待 WiFi 获得 IP（有限次重试），随后启动 esp-mqtt 客户端。
 * 若热点不可达，内部按策略重试；Broker 连不上一律不阻塞轮询。
 *
 * @return ESP_OK 成功；否则见 esp_wifi/esp_mqtt 错误码（不 fatal）。
 */
esp_err_t wifi_mqtt_init(void);

/**
 * @brief 把 SoundDog 5×Holding 快照打包成 JSON 并发布到 Broker。
 *
 * 直接读入参指向的 holding_regs（与 01 打印同一份内存，不复制字段）。
 * 非阻塞：esp_mqtt_client_publish 为异步入队；断线（未连接）时跳过发布。
 *
 * @param regs 指向 holding_regs 的指针，不得为 NULL。
 */
void gateway_mqtt_publish(const sounddog_holding_t *regs, uint16_t ok_count);

/* ------------------------------------------------------------------ */
/* FEAT-ESPGW-05：MQTT 下行控制（订阅 cmd → 回调 → 回执 ack）          */
/* ------------------------------------------------------------------ */

/**
 * @brief 下行控制请求。cid = SoundDog 保持寄存器号（0..MASTER_PARAM_CNT-1）。
 *
 * 线路格式（纯文本，⛔ 不引 JSON 解析库）：
 *   下行  <topic_cmd> : {"cid":0,"value":650}
 *   回执  <topic_ack> : {"cid":0,"value":650,"err":0}
 */
typedef struct {
    uint16_t cid;      /* 寄存器号 */
    uint16_t value;    /* 目标值 */
} gateway_cmd_t;

/**
 * @brief 下行回调类型。⚠️ 在【MQTT 任务上下文】被调用。
 *
 * ⛔ GW-04：回调里**不得**直接调 mbc_master_set_parameter() 等主站 API
 * （唯一任务独占）。正确做法 = 入队，由 master_poll_task 执行。
 */
typedef void (*gateway_cmd_cb_t)(const gateway_cmd_t *cmd);

/**
 * @brief 注册下行回调（必须在 wifi_mqtt_init() 之前调用）。
 *
 * 只有注册后才会在 MQTT_EVENT_CONNECTED 时订阅 cmd topic；
 * 未注册 = 纯上行模式（与 FEAT-ESPGW-02 行为一致）。
 */
void gateway_mqtt_register_cmd_handler(gateway_cmd_cb_t cb);

/**
 * @brief 上行一条写回执（异步、断线丢弃）。
 *
 * @param cid    寄存器号
 * @param value  实际下发的值
 * @param err    写入结果（esp_err_t 原值；ESP_OK=0）
 */
void gateway_mqtt_publish_ack(uint16_t cid, uint16_t value, int err);

#endif /* WIFI_MQTT_H */