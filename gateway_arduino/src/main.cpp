// SoundDog RS485 网关骨架 (ESP32-S3 N16R8 + Arduino + ModbusRTU + MQTT)
// 角色：RS485 (Modbus RTU) 主站  → 读 STM32 从站 holding 寄存器 → MQTT 上云
//
// 硬件接线（MAX485 / MAX3485 模块 → ESP32-S3）：
//   注意：用硬件 UART1 走 MAX485/MAX3485 模块（勿用 UART0 调试口）
//   MAX485 DI  → 板 UART1 TX (RS485_TX_PIN)  
//   MAX485 RO  → 板 UART1 RX (RS485_RX_PIN)
//   MAX485 DE+RE 短接 → 板 GPIO (RS485_RTS_PIN)，高发低收
//   MAX485 VCC/GND → 3.3V/GND（共地；禁止 5V，见 R36）
//
// ⚠️ 模块丝印语义（R42）：丝印 TX = 芯片 RO（模块向 MCU 发）→ 接 MCU RX；
//   丝印 RX = 芯片 DI（模块从 MCU 收）→ 接 MCU TX。必须 TTL 交叉！
//
// Modbus 从站（STM32 FreeModbus）：
//   - 从站地址 SLAVE_ID=1
//   - 0 基址（R34）：访问首个寄存器帧地址字段须发 0x0000
//   - 主站读 Holding 寄存器函数 0x03
// =============================================================
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif
#include <PubSubClient.h>
#include <ModbusRTU.h>          // emelianov/modbus     (兼容 ESP32/8266)
#include "config.h"             // 网络 + 寄存器 配置（见示例文件）

// ---- 硬件引脚（RS485 引脚在 config.h：RS485_TX/RX/RTS）----
#define DE_RE_PIN   RS485_RTS_PIN   // DE+RE 短接，高发低收
#define RTU_BAUD    115200          // 与 STM32 侧 USART3 一致
#define WIFI_MAX_RETRY  20          // 方案B：WiFi 有限重试次数(×500ms)，连不上不阻塞

// ---- 从站信息（SoundDog，见 config.h）----
const uint8_t SLAVE_ID = MB_SLAVE_ID;     // 从站地址
const uint16_t REG_HOLDING = MB_REG_TH;   // 0 基址起始寄存器索引

ModbusRTU mb;
HardwareSerial RS485Serial(1);          // UART1：RS485/Modbus 专用（与 UART0 日志分离）
WiFiClient espClient;
PubSubClient client(espClient);

uint16_t thValue = 0;          // 最近一次读到的值
unsigned long lastPub = 0;

// WiFi 已连、MQTT 已连时重连
bool mqttConnect();

void setup() {
  Serial.begin(115200);
  delay(100);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  // 方案B：有限重试（WIFI_MAX_RETRY * 500ms），连不上也不阻塞 Modbus 轮询
  int wretry = 0;
  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED && wretry++ < WIFI_MAX_RETRY) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\nWiFi connected, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[WARN] WiFi not connected; RS485 polling continues, MQTT pending");
  }

  client.setServer(MQTT_HOST, MQTT_PORT);

  // Modbus 主站，启用 RS-485 方向控制引脚
  pinMode(DE_RE_PIN, OUTPUT);
  digitalWrite(DE_RE_PIN, LOW);
  RS485Serial.begin(RTU_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  mb.begin(&RS485Serial, DE_RE_PIN);   // Modbus 走 UART1，DI/RO 接 MAX485
  mb.master();

  Serial.printf("Modbus master ready, slave=0x%02X reg(0-base)=0x%04X\n",
                SLAVE_ID, REG_HOLDING);
}

void loop() {
  // 1) 维护 WiFi：断开时退避重连，但不阻塞 Modbus 轮询（方案B 降级）
  static unsigned long lastWifiRetry = 0;
  if (WiFi.status() != WL_CONNECTED && (millis() - lastWifiRetry) >= 5000) {
    lastWifiRetry = millis();
    WiFi.disconnect();
    WiFi.reconnect();
  }
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  if (wifiOk && !client.connected()) {
    mqttConnect();
  }
  if (wifiOk) {
    client.loop();
  }

  // 2) 每 RTU_POLL_MS 读一次 holding 寄存器（异步，非阻塞）
  static unsigned long lastPoll = 0;
  if ((millis() - lastPoll) >= RTU_POLL_MS) {
    lastPoll = millis();
    mb.readHreg(SLAVE_ID, REG_HOLDING, &thValue, 1, [](Modbus::ResultCode event, uint16_t, void*) {
      if (event == Modbus::EX_SUCCESS) {
        Serial.printf("Hreg[0x%04X]=%u\n", REG_HOLDING, thValue);
      } else {
        Serial.printf("readHreg err: 0x%02X\n", (uint8_t)event);   // R34: 若异常越界先怀疑 0 基址
      }
    });
  }

  // 3) 定时发布到 MQTT
  if (wifiOk && (millis() - lastPub) >= MQTT_PUB_MS) {
    lastPub = millis();
    String payload = String("{\"reg\":") + String(REG_HOLDING) +
                     String(",\"val\":") + String(thValue) + String("}");
    client.publish(MQTT_TOPIC, payload.c_str());
    Serial.print("MQTT publish: "); Serial.println(payload);
  }

  mb.task();     // 驱动 Modbus 状态机（必须每循环调用）
  yield();       // WiFi watchdog
}

bool mqttConnect() {
  for (int i = 0; i < 5; i++) {
    if (client.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)) {
      Serial.println("MQTT connected");
      return true;
    }
    delay(1000);
  }
  Serial.println("MQTT connect failed");
  return false;
}

