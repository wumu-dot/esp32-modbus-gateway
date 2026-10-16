// =============================================================
// config.h — ESP32-S3 网关配置
// ⚠️ 真凭据不要放注释里；把本文件复制为 SecretConfig.h 并填入真值，
//    然后在 main.cpp 里 #include "SecretConfig.h"（或直接改下面宏）。
// =============================================================
#pragma once

// ---- WiFi ----
#define WIFI_SSID       "YOUR_WIFI_SSID"      // 采用 ASCII，>4 字符
#define WIFI_PASSWORD   "YOUR_WIFI_PASS"      // 采用 ASCII，>4 字符

// ---- MQTT Broker ----
#define MQTT_HOST       "192.168.1.100"       // 例：外网主机名或 IP
#define MQTT_PORT       1883
#define MQTT_USER       ""
#define MQTT_PASS       ""
#define MQTT_CLIENT_ID  "sounddog-gw"
#define MQTT_TOPIC      "sounddog/telemetry"

// ---- SoundDog 从站 ----
#define MB_SLAVE_ID     1          // STM32 FreeModbus 从站地址
#define MB_REG_TH       0x0000     // 0 基址：首个 holding 寄存器（R34）
#define RTU_POLL_MS     2000       // RS485 轮询周期
#define MQTT_PUB_MS     5000       // MQTT 发布周期
// ---- RS485 (UART1, MAX485/MAX3485) ----
#define RS485_TX_PIN    17          // 板 TX → MAX485 DI（模块收）
#define RS485_RX_PIN    18          // 板 RX ← MAX485 RO（模块发）
#define RS485_RTS_PIN   5           // DE+RE 短接 → 该引脚（高发低收）
