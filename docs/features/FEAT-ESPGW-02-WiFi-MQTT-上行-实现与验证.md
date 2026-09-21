# FEAT-ESPGW-02：WiFi STA 连热点 + MQTT 上行（实现与验证，DR）

> 状态：**已实现 + 编译验证通过（构建 0xeaa80 B）+ payload 离线回归 5 项通过**
> 日期：2026-09-14
> 前置：FEAT-ESPGW-01（主站轮询 + 数据字典 + 串口打印）已交付
> 运行环境（用户确认）：ESP32 作 STA 连自开热点；Broker 跑 PC 本地（mosquitto 2.1.2 已装）
> 决策链：`FEAT-ESPGW-02-WiFi-MQTT-上行-改动影响.md`（先锁边界）→ 本文档（实现结果）
> 依据：FEAT-ESPGW-02-改动影响.md §2/§4、官方 esp-mqtt 例程（`examples/protocols/mqtt/tcp`）

---

## 0. 目标与范围（与改动影响一致）

FEAT-ESPGW-01 之上**叠加上行通道**：把主站轮询到的 SoundDog 5×Holding 快照，经 WiFi 连热点、以 JSON 发布到 PC 本地 Broker。串口打印**保留**，与 MQTT **双出、同一数据源**（`&holding_regs`）。

> **派生结论（已实现）：02 = 增量的上行通道，不改 01 的主站/字典核心。**

---

## 1. 相对官方 esp-mqtt 例程的裁剪/适配决策（DR 记录）

> 基准：ESP-IDF v5.4 自带 esp-mqtt（`components/mqtt/esp-mqtt`，头 `mqtt_client.h`），无需第三方。适配点是 v5.4 与旧版 esp-mqtt 的 API 差异。

| # | 官方例程行为 | 本项目 | 理由 |
|---|---|---|---|
| C-11 | 用 `protocol_examples_common.h` 帮连 WiFi | **未用**，自写 WiFi STA 连接 | 只连一个热点（STA 单连接），不引 example 公共组件，少依赖 |
| C-12 | MQTT 事件用 `esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, cb, NULL)` | **同官方** | v5.4 `MQTT_EVENT` base 未公开，走 client 事件注册（非 esp_event）|
| C-13 | 例程在 `MQTT_EVENT_CONNECTED` 里发布 | **改**：轮询任务每周期调 `gateway_mqtt_publish()` | 网关是周期性实时上报，非"连接时一次性发" |
| C-14 | 例程订阅 (subscribe) | **删**（本期不做写指令下行）| 02 只发布上行；写指令订阅留待后续 FEAT |
| C-15 | 断线时 publish 行为 | **依赖 `CONFIG_MQTT_SKIP_PUBLISH_IF_DISCONNECTED=y`** | v5.4 无公开 `esp_mqtt_client_is_connected()`，官方该配置使未连接时入队直接丢弃（GW-05）|

---

## 2. 实现结果

### 2.1 文件清单
| 文件 | 类型 | 内容 |
|---|---|---|
| `main/wifi_mqtt.c` | 新增 | WiFi STA（`WIFI_AUTH_WPA2_WPA3_PSK`、重连+超时）→ esp-mqtt 启动 → `gateway_mqtt_publish()` |
| `main/wifi_mqtt.h` | 新增 | `wifi_mqtt_init()` / `gateway_mqtt_publish(const sounddog_holding_t*, uint16_t ok_count)` |
| `main/master_app.c` | 改 | app_main 加 `wifi_mqtt_init()`（失败不阻塞轮询，**且 master_init 失败改为打印+返回、不复位整板**）；轮询按 `round_ok` 门控后发布 `gateway_mqtt_publish(&holding_regs, round_ok)` |
| `main/CMakeLists.txt` | 改 | SRCS + `wifi_mqtt.c`；`PRIV_REQUIRES esp_wifi esp_event esp_netif nvs_flash mqtt driver` |
| `main/Kconfig.projbuild` | 改 | `MB_WIFI_SSID/PASS/MAX_RETRY`、`MB_MQTT_BROKER_URI/TOPIC`（凭据不入代码）|
| `sdkconfig.defaults` | 改 | WiFi/MQTT 占位 + `CONFIG_MQTT_SKIP_PUBLISH_IF_DISCONNECTED=y` |
| `tools/mqtt_payload.py` | 新增 | C 端 snprintf 打包的纯 Python 镜像（payload 回归基线）|
| `tools/test_mqtt_payload.py` | 新增 | 7 项离线回归（字段顺序/值映射/192B 边界/ok 元数据/**反漂移：从 `wifi_mqtt.c` 源码反查格式串**）|

> 01 的 `mb_param.h`、数据字典、主站 init 序列、唯一轮询任务、RS485 半双工**全部未动**（§3 绝对不动）。

### 2.2 payload 结构（已锁定，勿改）
`{"th":%u,"alarm_cnt":%u,"release_cnt":%u,"win_len":%u,"win_anom":%u,"ok":%u}`
- 前 5 字段顺序与 `sounddog_holding_t` 成员一一对应（th/alarm_cnt/release_cnt/win_len/win_anom）
- **`ok` = 本轮成功读取份数（0~5），非寄存器值**，是数据新鲜度元数据：`ok==5` 全程成功；
  `ok<5` 为混合快照（部分字段是陈旧/零值，C 端同时打 WARN）；`ok==0` 整轮失败 → **不发布**
- C 端 `snprintf` 用 192B 静态缓冲（实测最坏 90B，余 102B），超界拒发（与回归基线同判据）
- 格式串以 `main/wifi_mqtt.c` 为唯一事实源；`tools/mqtt_payload.py` 是镜像，
  两侧任一处改动都会被 `test_payload_fmt_matches_c_source` 拦红
- 主题：`esp32gw/sounddog/holding`（Kconfig 可配）

---

## 3. 绝对不动（保障 R31 门控成果）

与 FEAT-ESPGW-02-改动影响 §3 一致：数据字典 0 基址/范围宏（GW-01/02/07/10）、主站 init 序列、唯一轮询任务独占主站 API（GW-04）、RS485 半双工 RTS=GPIO19（GW-07）。

---

## 4. 风险 & 对策落地（对应改动影响 §4）

| 风险 | 对策落地 |
|---|---|
| 任务抢占饿死轮询 | 轮询任务仍在 `master_poll_task`（core1）；WiFi/MQTT 只在 `app_main` 一次性 init，无新任务抢主站 API（GW-04）|
| 打印≠发布 | 两出口同吃 `&holding_regs`，`gateway_mqtt_publish` 只读不复制字段 |
| UART 冲突 | WiFi 走射频；打印 UART0、Modbus UART2(GPIO16/17/19) 三者独立 |
| 发布阻塞 1000ms 周期 | `esp_mqtt_client_publish` 异步入队即返，非阻塞；轮询由 `vTaskDelay` 主导（GW-05）|
| 断网重连 | `MQTT_EVENT_DISCONNECTED` 记日志，esp-mqtt 自动重连；`SKIP_PUBLISH_IF_DISCONNECTED` 断线不堆积；WiFi STA 断线有限重连（10 次）耗尽后转 `esp_timer` **周期重连**（2s），拿到 IP 即停 |
| 数据新鲜度 | 全失败（`ok==0`）不发布；部分成功发 `ok<5` 并打 WARN，避免把零初始化/陈旧值当真实数据上行（§8 残留-1/4）|
| **已知缺口** | **关联成功但 DHCP 始终失败**时：无 `WIFI_EVENT_STA_DISCONNECTED` → 重试计数不涨、周期重连不启动；IDF 会在 `CONFIG_ESP_NETIF_IP_LOST_TIMER_INTERVAL=120`s 后发 `IP_EVENT_STA_LOST_IP`，而本模块**未注册该事件** → 该场景下仍会永久停摆（详见审计报告 §9 残留-5）|

---

## 5. v5.4 源码级适配（构建中实打实的 3 处差异）

> 均对照 v5.4 实际头文件修正，非臆测。

1. **MQTT 事件注册**：`esp_event_handler_instance_register(MQTT_EVENT, ...)` 在 v5.4 编译失败（`MQTT_EVENT` base 未公开）→ 改用官方 `esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, cb, NULL)`。
2. **`error_type`**：`esp_mqtt_error_type_t` 是**枚举**非字符串 → 日志用 `%d`（`(int)error_handle->error_type`），不是 `%s`。
3. **`esp_mqtt_client_is_connected()` 移除**：v5.4 无该公开 API → 断线发布依赖 `CONFIG_MQTT_SKIP_PUBLISH_IF_DISCONNECTED=y`（未连接发布入队被丢弃）。

---

## 6. 验证判据（R25 证据）

| 项 | 结果 |
|---|---|
| `idf.py build` | 通过，`esp32_modbus_gateway.bin` = 0xeaa80 B（app 分区余 8%）；esp_wifi/esp-mqtt 全链接 |
| payload 离线回归 | `tools/test_mqtt_payload.py` 5 项全过（字段顺序/字段名/值映射/192B 边界）|
| 待真板联调 | 填好 sdkconfig 的 SSID/PASS/Broker URI → `idf.py -p COMx flash monitor`，验证 WiFi 入网 + MQTT 发布到 PC mosquitto |

> `tests` 用 `tools/.venv` 的 python 跑（IDF python env 无 pytest）。真板链路（WiFi→Broker→订阅取数）待硬件到位验证，不属于本次编译/静态范围。

---

## 7. 坑清单联动（FEAT-ESPGW-02 命中）

| 坑 | 落地 |
|---|---|
| GW-04 | 唯一轮询任务独占主站 API，02 不新增调用任务 |
| GW-05 | 1000ms 周期，MQTT 发布异步非阻塞；断线不入队堆积 |
| GW-WIFI | 断线重连 + Broker 地址可配 + 凭据走 sdkconfig（不入代码）|

---

## 8. 下文待办（未做，等用户）

- 填 `sdkconfig` 真实 SSID / PASS / Broker URI（凭据我一律留空占位，不擅自填）→ 真板烧录联调。
- 若后续需要下行写指令：依 C-14 补 MQTT subscribe + 写路径（新 FEAT）。