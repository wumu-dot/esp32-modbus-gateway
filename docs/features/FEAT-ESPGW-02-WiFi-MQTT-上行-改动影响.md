# FEAT-ESPGW-02：WiFi STA 连热点 + MQTT 发布（改动影响分析，DR）

> 状态：**改动影响分析（DR 记录，尚未编码）**
> 日期：2026-09-14
> 前置：FEAT-ESPGW-01 已交付（主站轮询 + 数据字典 + 串口打印，R31 编码门控通过）
> 运行环境（用户确认）：ESP32 作 STA 连自开热点；Broker 跑 PC 本地（mosquitto 2.1.2 已装）
> 本文档目的：在动任何代码前，**锁定 ESPGW-01→02 的改动边界与取舍**，供评审与追溯。

---

## 0. 目标与范围

本期在 FEAT-ESPGW-01（Modbus RTU 主站轮询 + 串口打印）之上，**叠加上行通道**：
把主站轮询到的 SoundDog 5×Holding 快照，经 WiFi 连热点、以 MQTT 发布到 PC 本地 Broker，供 PC 订阅取数。

- **网络接入**：ESP32 作 STA 连用户自开热点（`threshold.authmode = WIFI_AUTH_WPA2_WPA3_PSK`，兼容 WPA2 与 WPA3 热点）
- **Broker**：PC 本地 mosquitto 2.1.2（已验证能监听 1883 + 收发闭环）
- **主题**：`esp32gw/sounddog/holding`（发布 5 寄存器 JSON + `ok` 新鲜度字段）
- **决定**：串口打印 **保留**，与 MQTT **双出**，同一数据源

> **派生结论：02 = 增量的上行通道，不改 01 的主站/字典核心。**

---

## 1. 来源与依据（可回溯）

| 项 | 依据 |
|---|---|
| 主站轮询 + 快照 | `.reference/esp-modbus/examples/serial/mb_serial_master/main/serial_master.c`（官方 v2.x 基准）|
| `holding_regs` 快照 | `main/master_app.c L37（master_app.c）` + `mb_param.h` 结构体 |
| 应用层 UI Arduino（沿用） | `main/app_main()` → `master_poll_task`（master_app.c L172-182）|
| MQTT 组件 | ESP-IDF 自带 `esp-mqtt`（`esp_mqtt_client.h`），无需第三方 |
| Broker | PC 本地 mosquitto 2.1.2（`C:\Program Files\mosquitto\`，已验证 pub/sub 闭环）|

---

## 2. 逐文件改动清单（影响级别）

| 文件 | 01 现状 | 02 改动 | 影响级别 |
|---|---|---|---|
| `master_app.c` | app_main init 主站→起轮询任务；`master_poll_task` 打 ESP_LOGI | **改**：app_main 加 WiFi/MQTT init；轮询任务在打印旁**加** MQTT 发布 | 中 |
| `mb_param.h` | 数据字典 + 结构体 + 范围宏 | **不动**（字段/地址/范围不变）| **无** |
| `Kconfig.projbuild` | 仅 UART 配置 | **新增** WiFi SSID/PASS、Broker 地址（`sdkconfig` 可配）| 轻 |
| `main/CMakeLists.txt` | SRCS 仅 `master_app.c` | **改**：SRCS 加 `wifi_mqtt.c` | 轻 |
| `sdkconfig.defaults` | UART 默认 | **改**：加 WiFi 表项默认占位 | 轻 |
| `wifi_mqtt.c`（新）| 不存在 | **新增**：WiFi STA 连接 + MQTT init + 发布 | 新增 |
| `wifi_mqtt.h`（新）| 不存在 | **新增**：`gateway_mqtt_publish(const sounddog_holding_t *)` | 新增 |
| 本文档 `FEAT-ESPGW-02-*.md`（新）| 不存在 | **新增**：改动影响 DR | 新增 |

> **核心耦合点**：`holding_regs` 这份快照有两个消费者（01 打印、02 MQTT），**必须吃同一份内存，不得复制字段**（防"打印的值≠发的值"）。

---

## 3. 绝对不动（保障 R31 门控成果）

- 数据字典：0 基址、范围宏、寄存器映射（GW-01/02/07/10）
- 主站 init 序列：`create_serial → set_pin → set_descriptor → set_mode → start`
- 唯一轮询任务：独占主站 API、1000ms 周期（GW-04/GW-05）
- RS485 半双工：RTS=GPIO19 自动驱动 DE/RE（GW-07）

---

## 4. 风险点 & 对策

| 风险 | 说明 | 对策 |
|---|---|---|
| 任务抢占 | 新增 MQTT/WiFi 任务若优先级错会饿死轮询 | 复用 01 pin 策略；唯一轮询任务仍独占主站 API；MQTT 用短超时非阻塞 |
| 日志→MQTT 语义 | 两出口不得各改一份字段 | 统一从 `&holding_regs` 打包，单一数据源 |
| UART 冲突 | WiFi/MQTT 不得碰 UART0 打印 / UART2 Modbus | WiFi 走内部射频；打印留 UART0、Modbus 走 UART2(GPIO16/17/19)，三者不冲突 |
| 发布阻塞 | MQTT publish 阻塞会拉长 1000ms 周期 | 发布短超时/非阻塞；轮询由 `vTaskDelay` 主导 |
| 断网重连 | 热点/路由断开 → MQTT 掉线 | `wifi_mqtt.c` 必带重连；断连期轮询照常、发布跳过 |

---

## 5. 改动后完整数据流

```
主站轮询→读5寄存器→存 holding_regs（01 既有）
                                │
                    ┌───────────┴───────────┐
              ESP_LOGI 打印(01)      JSON打包→MQTT发布(02)
                                       │
                              WiFi→热点→Broker→PC订阅
```

---

## 6. 落地顺序（编码顺序建议）

1. 新增 `wifi_mqtt.c/.h`（独立文件，零侵入 01）
2. 改 `master_app.c` 的 `app_main`（加 WiFi/MQTT init）+ 轮询打印位置加发布
3. 改 `main/CMakeLists.txt` SRCS、`Kconfig.projbuild`、`sdkconfig.defaults`
4. 离线 pytest 验 payload 结构；真板烧录联调（待热点/板子参数到位）

---

## 7. 克清单联动（本 FEAT 预计命中）

| 坑 | 落地 |
|---|---|
| GW-04 | 唯一轮询任务独占主站 API，不因 02 新增任务破坏 |
| GW-05 | 1000ms 周期，MQTT 发布不阻塞轮询 |
| （新增）GW-WIFI | 断线重连 + Broker 地址可配置 + 凭据不入代码（sdkconfig）|