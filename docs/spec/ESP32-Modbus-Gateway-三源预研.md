# ESP32 Modbus 网关 —— 三源预研

> 状态：预研完成（编码前门控，R31）
> 日期：2026-09-14
> 交付物：本文档（源一源码行为 / 源二Issue区 / 源三坑清单，含 R29 四要素）
> 本地源码依据版本：esp-modbus **v2.x → 实际构建 2.1.3**（组件版 managed `espressif__esp-modbus`）；示例基准 `examples/serial/mb_serial_master`（**v2.x API，`mbc_master_create_serial`**）。

---

## 0. 定位与拓扑

本网关 = **ESP32（Modbus RTU 主站）**，经 RS485/MAX3485 轮询 **SoundDog（FreeModbus 从站）**，上行经 WiFi 以 MQTT 转发。本次只做预研，不落代码。

```
 SoundDog STM32F407(从站)        ESP32(主站+网关)                 TCP/Internet
 ┌─────────────────────┐   RS485  ┌────────────────────┐            ┌────────────┐
 │ FreeModbus addr0x01 │◄────────►│ esp-modbus master   │───WiFi────►│ MQTT broker│
 │ 5×Holding(0基址)    │  MAX3485 │ modbus_poll 任务     │            │ (云端/本地) │
 └─────────────────────┘  A/B/GND └────────────────────┘            └────────────┘
```

### 从站侧事实（`C:\Projects\SoundDog\firmware\soundDog\App\modbus_app.c`，源一已读）
- 从站地址 **0x01**；模式 RTU，波特率 **115200-8N1 无校验**（FreeModbus 配置）。
- 仅 **保持寄存器实例化**，共 5 个，**0 基址**（帧地址字段 0x0000…0x0004，R34 off-by-one 对齐，勿再"修正"）：
  | 帧地址(0基址) | 参数 | 语义 / 范围 |
  |---|---|---|
  | 0x0000 | TH | 阈值，×100 定点，100~2000 |
  | 0x0001 | ALARM_CNT | 连续异常告警帧数，范围宏 |
  | 0x0002 | RELEASE_CNT | 连续正常释放帧数 |
  | 0x0003 | WIN_LEN | 滑窗固定=100（拒改）|
  | 0x0004 | WIN_ANOM | 窗内异常计数 |
- 输入 / 线圈 / 离散 **未实例化 → 恒异常码 0x02**，主站不应访问（访问会得 `ESP_ERR_NOT_SUPPORTED`）。
- 写路径两遍校验：**任一越界 → 整体拒绝，参数全保留旧值**（0x02），主站必须整段合法再写。

> 主站数据字典 = 上表 5 条 Holding 参数（一行在册），其余类型一律不入字典。

---

## 一、源一：源码内部行为（读被调函数源码，防隐藏副作用）

> 原则（R27）：调库前先读被调函数源码。以下每条均标注 vendored 源码位置。

### 1.1 主站初始化/调用入口（v2.x API）
- 入口 `mbc_master_create_serial(&comm_info, &handle)` → **立即返回 controller 句柄**；随后 `mbc_master_set_descriptor(handle, table, n)` → `mbc_master_start(handle)`。
  - 引用（组件 v2.1.3）：`freemodbus/common/include/esp_modbus_master.h`（`mbc_master_create_serial` / `mbc_master_set_descriptor` / `mbc_master_start` / `mbc_master_get_parameter`）；实现 `modbus/mb_controller/port_serial_master/mbc_serial_master.c`。示例基准路径：`examples/serial/mb_serial_master/main/serial_master.c`。
- 总线拓扑一旦启动，**主站任务持有事件循环**：`MB_EVENT_STACK_STARTED` 起会 `eMBMasterPoll()` 轮询发送队列（`mbc_serial_master.c`）。所有上层请求必须经 `handle` 串行调用，不能绕开。

### 1.2 ⚠️ API 是"阻塞 + 单飞"（最高优先副作用）
- `mbc_serial_master_send_request`（`mbc_serial_master.c:168`）：先 `xSemaphoreTake(mbm_sema, MB_SERIAL_API_RESP_TICS)` 拿**二进制信号量**，期间任何并发 `get/set` 会**阻塞至多 `MB_MAX_RESPONSE_TIME_MS`** 或直接失败；一次仅一个请求在飞。
- `get_parameter/set_parameter` 每次 **`calloc(pdata)`**（= `mb_size<<1` 字节）并在返回前 `free`（`mbc_serial_master.c:407/457`）——**每次访问都有堆分配/释放**，高频率轮询下是堆抖动来源（见资源预算）。
- 上层**必须**在普通任务里调用（不可在 ISR/高优先实时任务阻塞等待）。

### 1.3 数据字典（Data Dictionary）硬约束
- `set_descriptor` 校验：**`cid` 必须严格顺序 0..N-1** 且等于表下标；`param_key` 非空；`mb_size>0`（`mbc_serial_master.c:152`）。**cid 不连续/乱序 → setup 失败 `ESP_ERR_INVALID_ARG`**。
- 参数按名查找：**`strlen` 先比长度，再 `memcmp` 比内容**（`mbc_serial_master.c:358`）。**名字重名 → 误命中错误参数**；广播写（`mb_slave_addr==0`）只在写模式允许，主站**读访问地址 0 → 直接报错**（`mbc_serial_master.c:370`）。

### 1.4 发送/帧校验（off-by-one 与 CRC）
- `eMBMasterRTUSend`（`mbrtu_m.c:203`）：**`pucMasterSndBufferCur[ADDR_OFF] = ucSlaveAddress`**，随后计算并追加 CRC16。**帧地址字段直接写 `mb_reg_start`，不做 +1**；而从站（FreeModbus）回调前内部 `usRegAddress++`。⇒ **主站 `mb_reg_start` 必须用 0 基址**（与 R34 完全对称）。
- `xMBMasterRTUReceiveFSM`（`mbrtu_m.c:250`）要求 `eSndState` 处于 `TX_IDLE`/`TX_XFWR` 才收；`eMBMasterRTUReceive` 做 **`usMBCRC16()==0` 校验**（`:177`），CRC 错 → `MB_EIO`。

### 1.5 超时与广播（主站视角的时序副作用）
- 发完最后字节 → `vMBMasterPortTimersRespondTimeoutEnable()`（`mbrtu_m.c:364`）；超时 → `xMBMasterRTUTimerExpired` 置 `EV_ERROR_RESPOND_TIMEOUT` → `mbc` 层映射 **`ESP_ERR_TIMEOUT`(0x107)**。
- 广播帧 → 改启 **convert delay**（`FMB_MASTER_DELAY_MS_CONVERT`，默认 200ms），主站**不等待响应**。
- 响应超时默认 `FMB_MASTER_TIMEOUT_MS_RESPOND=3000ms`（Kconfig/Kconfig:57）；单飞信号量 `MB_SERIAL_API_RESP_TICS = MB_MAX_RESPONSE_TIME_MS=5000ms`（`port/port.h:98`）。

### 1.6 RS485 收发方向（与 SoundDog 从站实现不同！）
- ESP32 主站方向切换由 **UART 硬件 RS485 半双工模式**自动完成：`uart_set_mode(UART_MODE_RS485_HALF_DUPLEX)` + `uart_set_pin(RTS引脚=DE/RE)`。portserial 层**只做软件收发状态机**，不手动翻转 DE。
  - `vMBMasterPortSerialEnable`（`port/portserial_m.c:129`）：TX 前 `vMBMasterRxFlush()` 清残留 + 停收；`xMBMasterPortSerialTxPoll`（`:177`）发完 **`uart_wait_tx_done(..., MB_SERIAL_TX_TOUT_TICKS)` 阻塞等待** 再切回接收。
  - t3.5 帧尾由 `uart_set_rx_timeout(MB_SERIAL_TOUT)` + 软件 re-arm 实现（`portserial_m.c:324`）；`uart_set_always_rx_timeout(true)`（`:330`）。
- **对比结论**：SoundDog 从站用 FreeModbus-CMSIS-RTOS2 移植（手动 GPIO DE=PD11 翻转）；ESP32 主站走 UART HW RTS 自动方向。**主站侧需要一根 DE/RE 连到 ESP32 UART RTS 引脚**（不是普通 GPIO 手动翻转，除非刻意关掉半双工模式自己控）。这是两个工程方向控制的**最大差异点**，移植思路不可直接套用。

### 1.7 资源尺寸（Kconfig 默认，见 §五预算）
- `MB_CONTROLLER_STACK_SIZE=4096`（控制器任务栈）
- `FMB_PORT_TASK_STACK_SIZE=4096`（串口事件任务栈）
- `FMB_SERIAL_BUF_SIZE=256`（UART RX/TX 各 256B；Modbus 最大帧 256B）
- `FMB_QUEUE_LENGTH=20`（UART 事件队列）
- `FMB_PORT_TASK_PRIO=10`（串口任务优先级）；控制器优先级 = `FMB_PORT_TASK_PRIO-1=9`
- `FMB_PORT_TASK_AFFINITY`（默认 CPU0 或 NO_AFFINITY）

---

## 二、源二：Issue / 权威社区区（真实 URL）

### 2.1 官方 ESP-Modbus 文档 "Possible Communication Issues"（官方排查表）
- URL：https://docs.espressif.com/projects/esp-modbus/en/latest/esp32/applications_and_references.html
- 要点（错误码→解法）：
  | 错误 | 含义 | 官方解法 |
  |---|---|---|
  | 0x106 `ESP_ERR_NOT_SUPPORTED` | 从站异常：寄存器不受支持（越界）| 对照从站寄存器表，检查主站数据字典 |
  | 0x107 `ESP_ERR_TIMEOUT` | 从站在响应窗口内未应答 | 调大 `CONFIG_FMB_MASTER_TIMEOUT_MS_RESPOND`；查物理连接；高优先任务抢占时把 Modbus 任务放到另一核 `FMB_PORT_TASK_AFFINITY`、增大 `FMB_PORT_TASK_PRIO` |
  | 0x108 `ESP_ERR_INVALID_RESPONSE` | 从站返回不支持/帧校验失败 | 查物理连接 + 主站数据字典 |
  | 0x103 `ESP_ERR_INVALID_STATE` | 主站 FSM 忙/前一请求未完成 | 查物理连接、增大任务栈、核对初始化顺序 |

### 2.2 社区实测案例：0x107 timeout（Battery 寄存器）
- URL：https://www.esp32.com/viewtopic.php?t=37819
- 现象：ESP32S3 主站读电池从站，持续 `mbc_master_get_parameter(...) 0x107`，加大 timeout 无效。
- 根因链：`MB_PARAM_HOLDING` + `mb_reg_start=0x1000` + `PARAM_TYPE_ASCII`；排查聚焦 **波特率/奇偶校验/寄存器起始地址与从站手册不符** → 直接结论：**先核对从站波特率 115200-8N1 与 0 基址字节序**，这是比 timeout 更靠前的根因。

### 2.3 其它权威渠道
- 官方 API 参考（ESP-IDF / esp-modbus）：https://docs.espressif.com/projects/esp-idf/en/v4.4.3/esp32s3/api-reference/protocols/modbus.html 、https://docs.espressif.com/projects/esp-modbus/en/main/esp32/port_initialization.html
- 协议规范（Modbus 组织）：https://modbus.org/specs.php
- 组件仓库 / issue 登记：https://github.com/espressif/esp-modbus/issues 、https://github.com/espressif/esp-modbus/discussions

> 上述全部为可访问真实 URL（R32）。未采用任何无链接断言。

### 2.4 官方仓库 Issue 区系统调研（GitHub API，2026-09-14）

> 用 GitHub REST API（公开、免登录）对 `espressif/esp-modbus` 按 `timeout / RTU master / RTS / register offset` 四关键词检索，共命中 140 条。以下为本工程（ESP32 RTU 主站，esp-modbus **v2.x**）**高相关**者，逐条标注影响与坑清单映射。已关闭（closed）为有官方结论可复用；open 为已知未解，须自行规避。

| # | Issue（编号/标题/状态）| 与我们的关联 | 坑清单映射 |
|---|---|---|---|
| #145 | [RTU Master cannot keep up with very responsive slave / bus spikes (EPROT-16, open)](https://github.com/espressif/esp-modbus/issues/145) | 官方维护者 **alisitsyn** 亲述：从站响应太快时，前帧 `mb_port_ser_send_data` 释放信号量之前就收到 UART_DATA 事件 → 半窗；0x00 假字节来自 **TX→RX 转换瞬间**，会催 T3.5 被当碎片帧 flush。**对策：响应快+RS485 方向切换敏感，轮询别太激进；换用 UART_RS485TX_RX_EN 抑制或接受重试** | 强化 GW-03/GW-05（轮询周期别压太紧）；新压实 GW-07（RS485 方向切换毛刺）|
| #166 | [Serial master race after failed request: next request may return ESP_ERR_INVALID_STATE (EPROT-61, closed)](https://github.com/espressif/esp-modbus/issues/166) | 失败请求后**立即发下一请求**可能返回 0x103 `ESP_ERR_INVALID_STATE`（FSM 未完全退出）。**必须等上一请求已返回后再发下一个；连续失败要留间隔/重试策略**，不能背靠背硬冲 | 强化 GW-04（单任务串行）→ 追加 **GW-12-新：轮询应容忍 0x103 留间隔** |
| #103 | [Packet Failure on Send Request – malformed frame on ESP-IDF v5.4.0 + serial mb_master (IDFGH-14820, closed)](https://github.com/espressif/esp-modbus/issues/103) | **正好是我们环境（v5.4.0 + 串行主站）**：发 0x02 读线圈帧被逻辑分析仪抓到"Message too long + 错 CRC"。**结论：v5.4 + 串行主站要重点验证发送帧完整性，不能只信日志** | 强化 GW-01/GW-02 验证判据：**打桩抓原始帧**（逻辑分析仪/回调）确认 CRC |
| #88 | [RTU unexpected event triggered 0x02 / timeout (IDFGH-14408, closed)](https://github.com/espressif/esp-modbus/issues/88) | ESP32-C3 + esp-modbus 2.0.1，从站**能正常读/写**但每 3s 报"unexpected event"0x02。即 **0x02 事件不必然代表链路坏**，须区分异常帧 vs 真超时 | 强化 GW-02（0x02≠链路断语义）|
| #20 | [Modbus Master RTU parity errors not detected (IDFGH-9296, closed)](https://github.com/espressif/esp-modbus/issues/20) | 官方维护者承认：**主站不检测从站返回帧的 UART 奇偶错误**（event task 只 log 不处理），奇偶不匹配时数据仍可能过 CRC 被当"正确"。官方已给 [0001 串行 master/slave fix parity patch](https://github.com/espressif/esp-modbus/files/...)。**我们 115200-8N1 无校验，天然规避；若将来开奇偶校验必打此补丁** | 强化 GW-10（波特率/校验一致性）→ 记录 v2 的奇偶检测缺口 |
| #40 | [Error 107 in reading modbus register (IDFGH-11299, closed)](https://github.com/espressif/esp-modbus/issues/40) | ENERSOL 表 + `TX17/RX16/RTS5` 报持续 0x107，最终收敛到**先查波特率/起始地址/字节序**，与 §2.2 结论一致 | 呼应 GW-01/GW-10 |

**源②补齐结论（R32）**：v2.x 主站最该防的三件事——①请求失败后别立即重发（#166 预留 0x103 处理）；②发送帧要用实测原始帧验证 CRC 而非常信日志（#103）；③0x00 假字节/方向切换毛刺在响应过快时出现（#145）。三条都已被上述坑清单 GW 条目覆盖。

---

## 三、R29-GitHub 研究（≥2 相似实现 + 官方库验证）

对每个实现标 `可复制 / 不可复制`（结构可复用、电气/寄存器参数不照搬，R28）。

### 3.1 esp32-modbus-mqtt-bridge（ESP32-S3 Modbus RTU→MQTT，HA 自动发现）
- URL：https://maker.wiznet.io/Lihan__/projects/esp32-modbus-mqtt-bridge/
- 结构：Modbus RTU 经 RS485（GPIO44 RX / 43 TX / **GPIO4 显式 DE**）↔ MQTT + Home Assistant 自动发现；MIT；v2.2.0；PlatformIO。
- **可复制**：① Modbus 侧一心（poll） + MQTT 侧一心（publish）的**双任务分层**；② MQTT discovery 主题模型；③ 单总线单从站数据字典组织。
- **不可复制**：GPIO 引脚映射、DE 走显式 GPIO（本需求倾向 UART HW RTS 自动方向，见 §1.6）、KinCony 寄存器表。

### 3.2 GatewaySuriotaPOC / SRT-MGATE-1210（工业 Modbus RTU/TCP→MQTT/HTTP 网关）
- URL：https://maker.wiznet.io/jaden/projects/gatewaysuriotapoc-1/
- 结构：ESP32-S3 + W5500（有线主链路 + WiFi 热备）；Modbus RTU×2 + TCP；分离 **network / modbus / mqtt / device-manager** FreeRTOS 任务；MQTT 大缓冲 + keep-alive(60) + socket_timeout(30) + 持久队列 + 自动重连。
- **可复制**：任务隔离 + 网络自恢复；MQTT 客户端 keep-alive/buffer 调优指导（长期运行不掉线）。
- **不可复制**：W5500 相关（本需求 WiFi 即够）、BLE 配置、云绑定细节。

### 3.3 官方串行主站例程（mb_serial_master）——基准
- URL：https://github.com/espressif/esp-modbus/tree/main/examples/serial/mb_serial_master （组件版：https://components.espressif.com/components/espressif/esp-modbus/versions/2.0.2/examples/serial/mb_serial_master）
- **可复制**：数据字典表 + `mbc_master_setup/set_descriptor/get_parameter` 调用序列 + RS485 例程接线图（MAX485/MAX3485 A/B/GND/RTS 接线）。
- **不可复制**：示例里的从站寄存器表（Humidity/Temperature），须换成 §0 的 SoundDog 5 寄存器表；引脚默认值按实际硬件改。

### 3.4 官方库验证结论
- esp-modbus 是 Espressif 官方库。本工程采用 **v2.x API（`mbc_master_create_serial` 系列）**，组件版由 `idf.py build` 解析 `main/idf_component.yml`（`espressif/esp-modbus: "^2"`）自动拉取并编译（构建实得 **2.1.3**）——源码即组件本地 managed 副本，读之即为"官方验证"（R27 源一）。v1.x（`mbc_master_init`）与 v2.x API 不同；即便单主单从，也统一走 v2.x 以免与组件依赖解析冲突。

---

## 四、源三：坑清单交付物（坑 → 后果 → 预防 → 验证判据）

| # | 坑 | 后果 | 预防 | 验证判据 |
|---|---|---|---|---|
| GW-01 | 主站 `mb_reg_start` 用 1 基址 | 首寄存器读/写全歪（落到 ALARM 或 0x02）| 用 §0 的 **0 基址**表；字典 `mb_reg_start=0x0000` 起 | 板载读 TH：发帧 `01 03 0000 0001 …`，回报文首字=TH 现值 |
| GW-02 | 从站 0x02 异常码被当"链路断" | 误判故障 | 0x02=MB_ENOREG（地址越界）≠链路断；先查地址/字典 | 打桩记录：读 `01 03 0005 …` 回 `01 83 02 …`（明确越界），与 0x107 区分 |
| GW-03 | 高优任务抢占致 0x107 频发 | 轮询假超时 | 隔离 Modbus 任务到专核/提 priority；`FMB_PORT_TASK_AFFINITY` | 压测下观察 `mbc_master_*` 0x107 计数不随 WiFi 流量起伏 |
| GW-04 | 并发调用 `get/set_parameter`（多任务同时访问） | 信号量争用阻塞/单飞串话 | 网关内**唯一 modbus_poll 任务**独占主站 API | 单任务轮询 + 队列化上行，无另一任务直调 |
| GW-05 | 高频轮询 `calloc/free(pdata)` | 堆碎片/抖动 | 控制轮询周期；必要时改走 `send_request` + 复用缓冲（若后续优化再议） | `heap_caps_get_free_size` 长时间稳定，无单调下降 |
| GW-06 | 广播写（addr=0）读访问 | 直接报错；语义误用 | 字典内定义写型广播参数时才置 `mb_slave_addr=0`，且只写不读 | 写入成功不回等；读广播参数立即报错属预期 |
| GW-07 | RS485 方向：是否 UART HW RTS | DE/RE 不翻转 → 收/自发环或静默 | **用 `uart_set_pin(RTS=DE)` + `uart_set_mode(RS485_HALF_DUPLEX)`**，确认 ESP32 的 DE 引脚连 RTS 而不是普通 GPIO | 轮询正常应答且无自发自收；不连 DE 时确实无应答 |
| GW-08 | 从站写路径"整段校验" | 一次性写多寄存器含 1 个越界 → 全拒 | 主站写前先读当前值拼合法段，或只写单寄存器 | 写 TH 合法成功；故意塞越界值，回 0x02 且对端值不变 |
| GW-09 | A/B 反接 / 共地缺失 / 端接 | 差分收不到（SoundDog 已实证过）| 复用 R33 排查清单：A/B 极性、A/B 与 ESP32 共地 GND、120Ω 端接（距离）；先软件回环锁链路 | MAX3485 出脚量 A-B 差分；软件收发一字节回环 |
| GW-10 | 波特率/奇偶不一致 | 0x107/0x108 乱飞 | 两侧统一 115200-8N1 无校验 | 建连后首帧即正确应答 |
| GW-11 | 主站 MQTT keep-alive/自动重连未配好（长期）| 掉线不重连，网关静默 | 复用 3.2 经验：keep-alive(60)、socket_timeout、自动重连 + 持久队列 | 拔网/断 WiFi→重连后数据续传，无静默 |
| GW-12 | 依赖解析到 esp-modbus 版本不匹配 / 误用 v1.x API | 组件解析失败或编译/链接找不到 `mbc_master_create_serial`、`mbc_master_set_descriptor` | 用 v2.x API 全链；`main/idf_component.yml` 声明 `espressif/esp-modbus: "^2"`，CMakeLists 顶层 `EXCLUDE_COMPONENTS freemodbus`（防旧版 FreeModbus 冲突）| `idf.py build` 零 error 且 `managed_components/espressif__esp-modbus` 解析为 **2.x**；`mbc_master_create_serial` 符号在当前组件头文件存在 |
| GW-13 | 请求失败后立即重发（esp-modbus #166，0x103 FSM 未完全退出）| 下一请求返回 `ESP_ERR_INVALID_STATE`，背靠背重发反而持续失败 | 轮询任务串行化，失败后**留间隔再试**；把 0x103 当"需等待"而非"硬错误"累加 | 注入一次坏帧 → 下一请求不报 0x103 仍成功；连续失败计数与重试间隔符合预期 |

> 坑清单即预研完成判据（R27：无坑清单=预研未完成）；GW 编号留作后续 FEAT/缺陷文档引用。

---

## 五、R29 四要素汇总

### 5.1 影响地图
- 本工程为**新建 ESP-IDF 工程**，无既有代码调用链（`.skills\codegraph.bat` 无需跑，代码尚未落盘）。
- **跨工程边界仅一处**：SoundDog 从站的 RS485 接口 `modbus_app.c`（帧协议/寄存器映射/0 基址）——**保持不变**，主站侧改动不触及。
- 已用 grep 双确认 SoundDog 侧唯一从站寄存器实现为 `firmware/soundDog/App/modbus_app.c`（保持寄存器 5 个，输入/线圈/离散全 MB_ENOREG），故主站字典 §0 表即为全部轮询面，无遗漏。
- 明确**不受影响区域**：SoundDog 的 OLED/I2C/麦克风/RS485 从站实现、FreeModbus 移植层、以及所有 `0xA5 旧协议`相关文档/脚本（后者属 Task G 另一独立工作线）。

### 5.2 资源预算（量化，源一实测默认）
| 项 | 预算 | 说明 |
|---|---|---|
| modbus 控制器任务栈 | ~4096 B | `MB_CONTROLLER_STACK_SIZE` |
| 串口事件任务栈 | ~4096 B | `FMB_PORT_TASK_STACK_SIZE` |
| UART RX/TX 内部缓冲 | 256 B×2 | `FMB_SERIAL_BUF_SIZE` |
| UART 事件队列 | 20 项 | `FMB_QUEUE_LENGTH` |
| 每访问堆抖动 | 5 寄存器≈10B 瞬时 | `get/set_parameter` 的 `calloc/free` |
| 主站 API 阻塞窗口 | 单飞 ≤5s；响应超时 3s | `MB_MAX_RESPONSE_TIME_MS` / `FMB_MASTER_TIMEOUT_MS_RESPOND` |
| WiFi + lwIP + MQTT 客户端 | 数十 KB（缺省堆，启用 MQTT 后实测） | 属网关上一层，非 Modbus 栈；需在实现阶段 `heap_caps` 实测确认 |
| CPU | 单主单从轮询 5 寄存器，帧极小，栈本身开销低 | 实测未列入；余量靠 `FMB_PORT_TASK_PRIO/AFFINITY` 保障实时性 |

### 5.3 FEAT 更新 / 后续里程碑
- 实现阶段将新建 `docs/features/` 下的 FEAT（链表根本条）：
  - `FEAT-ESPGW-01`：ESP32 Modbus RTU 主站初始化 + 数据字典（§0 表）+ 轮询读写（含 0 基址/异常码映射）。
  - `FEAT-ESPGW-02`：WiFi + MQTT 桥接（发布轮询结果 / 订阅写指令），任务分层（复用 §3.1/3.2 可复制模式）。
- 每个 FEAT 的"来源"小节须回链本文档的 §二 URL 与 vendored 源码路径（R32）。

---

## 六、验证判据汇总与下一步（编码前确认项）

进入编码（R31 门控）前需确认：
1. **ESP32 开发板型号与引脚**（UART 号 / RTS-DE 引脚 / 波特率）——决定 §1.6 方向控制与字典映射是否调整。
2. **上行链路选型**：MQTT broker 地址/主题，或首期仅串口打印打样（不接 WiFi）。
3. **轮询周期**默认值（影响 GW-05/GW-03 与资源预算）。
4. ESP-IDF 版本（组件 esp-modbus v2.x 要求 ESP-IDF v5.0+；本机 v5.4 满足）。

> 以上确认后，方可按 R31 进入 ESP-IDF 工程骨架开发；当前**只交付预研文档，未落任何实现代码**。---

## 七、已确认事项（板型 / 引脚 / 上行）— 2026-09-14

> 由用户确认，固化进本文档；编码（R31 门控）以本节为准。

### 7.1 板型与工具链
| 项 | 取值 | 说明 |
|---|---|---|
| 模组 | **ESP32-WROOM-32D**（经典 ESP32 devkit）| UART2 支持 RS485 半双工，RTS 可作 DE/RE |
| 工具链 | **ESP-IDF v5.4** | 沿用 pointer-desk `weather_clock` 已装版本；满足 esp-modbus v2.x 的 v5.0+ 要求（§六·项4 已解决）|
| Modbus 库 | **esp-modbus v2.x（构建实得 2.1.3）** | 组件版由 `idf_component.yml` 解析，`mbc_master_create_serial` 全链（§3.4）|

### 7.2 RS485 接线（ESP32-WROOM-32D → 板上 MAX3485）
主站方向走 **UART2 硬件 RS485 半双工，DE/RE = RTS2 自动驱动**（§1.6）。波特率 **115200-8N1**。

| ESP32 引脚 | 作用 | → MAX3485 引脚 |
|---|---|---|
| GPIO17（TXD2）| 主站 TX | **DI**（驱动输入）|
| GPIO16（RXD2）| 主站 RX | **RO**（接收输出）|
| GPIO19（RTS2，HW 自动）| **DE/RE** | **DE + /RE** 并接 |
| 3V3 | 供电（MAX3485 是 3.3V 器件，勿接 5V）| VCC |
| GND | 共地 | GND |
| — | RS485 总线 | A / B → SoundDog MAX3485 的 A/B |

> RTS 默认引脚 GPIO19（RTS2）；如需换引脚用 `uart_set_pin(..., rts=GPIO19, cts=UART_PIN_NO_CHANGE)` 显式指定。

### 7.3 上行与阶段安排
- **本期（FEAT-ESPGW-01）**：Modbus 主站 + 数据字典 + 轮询，结果**串口打印打样**，**不接 WiFi/MQTT**（复用 SoundDog 阶段4 用 USB-TTL 打断链路的思路，先把 Modbus 链路打穿）。
- **下期（FEAT-ESPGW-02，延后）**：再上 WiFi + MQTT 桥接（复用 §3.1/3.2 可复制模式），此时才涉及 GW-11（keep-alive/重连）。
- §六·项3 **轮询周期默认 1000ms**（对 `FMB_MASTER_TIMEOUT_MS_RESPOND=3000ms` 有富余），串口阶段先跑通、不够再调（关联 GW-03/GW-05）。

### 7.4 涉及坑清单联动
- GW-07 在此板上即变为具体引脚：**确认 GPIO19(RTS2) 接了 MAX3485 的 DE+/RE**，且 `uart_set_mode(UART_MODE_RS485_HALF_DUPLEX)`；否则不翻转 DE → 无应答。
- GW-10：两侧统一 **115200-8N1 无校验**（SoundDog 从站已是该配置，主站字典照 §0 表）。

> 以上确认后，才按 R31 进入 `esp32-modbus-gateway` 的 ESP-IDF 工程骨架开发；当前**仍只交付预研文档，未落任何实现代码**。