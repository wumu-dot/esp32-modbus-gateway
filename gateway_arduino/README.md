# SoundDog ESP32-S3 网关骨架（RS485 → MQTT）

> 分支：`feat/esp32s3-mqtt-gateway`（`C:\Projects\esp32-modbus-gateway`）
> 目标板：ESP32-S3-N16R8（16MB Flash + 8MB PSRAM opi），CH343 串口桥

ESP32-S3 作为 **RS485 (Modbus RTU) 主站**，轮询 STM32 从站（SoundDog），把结果发布到 MQTT 上云。
角色仅限 **RS485 主站 + WiFi + MQTT**；**端侧 AI 另立项，不在本骨架**。

---

## 1. 技术栈

- **框架**：Arduino（PlatformIO 构建）
- **Modbus**：`emelianov/modbus-esp8266` @4.1.0（兼容 ESP32/8266，RTU 主站）
- **MQTT**：`knolleary/PubSubClient` @2.8.0

## 2. 硬件接线（MAX485 / MAX3485 → ESP32-S3）

```
S3  UART1 TX (RS485_TX_PIN=17)  -> MAX485 DI       // 模块"收"数据输入（从 MCU 收）
S3  UART1 RX (RS485_RX_PIN=18)  -> MAX485 RO       // 模块"发"数据输出（发给 MCU）
S3  GPIO   (RS485_RTS_PIN=5)    -> MAX485 DE+RE(短接)  // 高=发 低=收
MAX485 VCC/GND -> 3.3V/GND（共地；禁止 5V，见 R36）
```

> 警告 丝印语义（R42）：线 TX = 芯片 RO（发向 MCU）-> 接 MCU RX；
> 丝印 RX = 芯片 DI（MCU 发来）-> 接 MCU TX。必须 TTL 交叉。
> 警告 用 UART1，勿用 UART0（日志口）。DE/RE 翻转由库托管。

## 3. 构建与烧录

```bash
pio run                  # 编译（无需接板）
pio device list          # 板插上后查 CH343 串口号
pio run -t upload        # 烧录
pio device monitor -b 115200   # 串口日志
```

> 真凭据：把 `src/config.h` 复制为 `src/SecretConfig.h` 填真值，
> 并 `#include "SecretConfig.h"`（该文件已被 `.gitignore` 排除，禁止入库）。

## 4. 行为设计（方案 B：有限重试 + 降级轮询）

- **WiFi**：`setup()` 有限重试 `WIFI_MAX_RETRY=20`(x500ms)，连不上不阻塞；
  `loop()` 断开时每 5s 退避重连，Modbus 轮询持续。
- **MQTT**：仅在 WiFi/MQTT 就绪时发布，否则本地轮询照跑、不外发。
- **Modbus**：非阻塞 `mb.readHreg()` 每 `RTU_POLL_MS` 轮询，0 基址（R34）。

## 5. 待办（硬件到位后）

- [ ] `config.h` 填真 WiFi/MQTT 凭据（SecretConfig.h）
- [ ] `pio run` 编译验证（R21：当前未验证）
- [ ] `pio run -t upload` 烧录 + MAX485 接线 + RS485 对打测试
- [ ] 上行契约定稿：骨架单字段 vs ESP-IDF 版 5 字段，需统一