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

#endif /* WIFI_MQTT_H */