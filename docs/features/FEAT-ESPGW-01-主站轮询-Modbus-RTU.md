# FEAT-ESPGW-01：ESP32 Modbus RTU 主站 + 数据字典 + 轮询（串口打样）

> 状态：**工程骨架开发中（R31 编码门控已通过）**
> 日期：2026-09-14
> 依赖：预研 `docs/spec/ESP32-Modbus-Gateway-三源预研.md`（§0 从站事实 / §7 板型引脚确认）
> 基准：Espressif 官方 `esp-modbus` main 分支 `examples/serial/mb_serial_master`（v2.x API）
> 本地基准：`.reference/esp-modbus/examples/serial/mb_serial_master`
> 从站唯一事实源：`C:\Projects\SoundDog\firmware\SoundDog\App\modbus_app.c` + `BSP\param.h`

---

## 0. 目标与范围

本期只打穿 **Modbus RTU 链路**（主站轮询 + 串口打印打样），**不接 WiFi/MQTT**（上行已由后续 **FEAT-ESPGW-02-WiFi-MQTT-上行-实现与验证.md** 承接，本文档只做 RTU 链路层）。参照预研 §7.3 阶段安排。

- **硬件**：ESP32-WROOM-32D；UART2；TX=GPIO17(RTS 见 §0.2)、RX=GPIO16、**RTS=GPIO19(DE/RE)**（预研 §7.2）
- **协议**：RTU 115200-8N1 无校验（预研 §7.4 / GW-10）
- **方向控制**：UART 硬件 RS485 半双工，RTS 自动驱动 DE/RE（预研 §1.6 / GW-07）
- **从站**：SoundDog addr=0x01，5×Holding，0 基址

> 数据字典 = 预研 §0 表 5 条 Holding（一行在册，其余类型一律不入字典）。

### 0.2 引脚（预研 §7.2 固化）
| ESP32 pin | 作用 | → MAX3485 |
|---|---|---|
| GPIO17 | TX | DI |
| GPIO16 | RX | RO |
| GPIO19 (RTS2) | DE/RE | DE + /RE |
| 3V3 / GND | 供电 / 共地 | VCC / GND |

---

## 1. 来源（R32 可溯回链）

| 层 | 来源 | 路径 |
|---|---|---|
| API（v2.x）| esp-modbus 官方组件（GitHub espressif/esp-modbus main）| `.reference/esp-modbus` |
| 主站例程基准 | 官方 `mb_serial_master` main | `.reference/esp-modbus/examples/serial/mb_serial_master/main/serial_master.c` |
| 依赖版本 | 官方 `idf_component.yml`：`espressif/esp-modbus: "^2"`，需 ESP-IDF ≥5.0 | 同例程目录 |
| 从站寄存器/范围 | SoundDog `modbus_app.c` + `param.h`（唯一事实源）| `C:\Projects\SoundDog\...\App\modbus_app.c` |
| KRconfig 默认 | 官方 `sdkconfig.defaults` + Kconfig | `.reference/esp-modbus/examples/serial/mb_serial_master/sdkconfig.defaults` |

> 官方例程在线确认：https://components.espressif.com/components/espressif/esp-modbus/versions/2.1.1/examples/serial/mb_serial_master （R32）

---

## 2. 相对官方例程的裁剪决策（DR 记录）

原则：**只保留官方骨架，做最小改动**，把不可复制内容（示例业务）剔除，换成 SoundDog 事实。

| # | 官方例程内容 | 裁剪决策 | 理由 |
|---|---|---|---|
| C-01 | `0x41` 自定义命令 handler（`mbc_set_handler`/`my_custom_handler`）| **删** | SoundDog 无自定义命令；省去 `MB_CUST_DATA_LEN` 缓冲与 handler 注册 |
| C-02 | `0x11 Report Slave ID` 读取 | **删** | 本期不做；去 CONFIG_FMB_CONTROLLER_SLAVE_ID_SUPPORT 依赖 |
| C-03 | `CONFIG_FMB_EXT_TYPE_SUPPORT` 下 U8/U32/Float/Double 扩展字典 | **删** | SoundDog 只有 u16 Holding；字典一行在册 |
| C-04 | 官方"Alarm/relay 业务"（`master_operation_func` 里 alarm_state / relay ON-OFF 判定）| **删** | 网关是轮询+上报，不做本机报警业务；改为纯轮询+打印 |
| C-05 | Poll 安排 | **改** | 官方 `UPDATE_CIDS_TIMEOUT_MS=500` + `POLL_TIMEOUT_MS=1` 快速测试逻辑；改**周期 1000ms** 轮询一次全部 5 寄存器（预研 §7.3 项3 已定）|
| C-06 | 官方例程 VFS Debug 日志开关 | **保留**（按需）| app_main 里 `esp_log_level_set` 对 modbus 组件启用 DEBUG 便于链路排查 |
| C-07 | 数据字典 | **换** | 官方 Humidity/Temperature 表 → SoundDog 5×Holding 表（§3）|
| C-08 | MB_DEVICE_ADDR | **换** | 官方默认 1，SoundDog 也是 1（addr0x01），保持不变但显式注释 |

> 每次裁剪都必须不破坏官方调用序列骨架：`mbc_master_create_serial → set_parameter 配置 → uart_set_pin → set_descriptor → uart_set_mode(RS485_HALF_DUPLEX) → start → 轮询`。

---

## 3. 数据字典（SoundDog 5×Holding，0 基址）

来源：预研 §0 表 + SoundDog `modbus_app.c:reg_off_to_pid` + `param.h` 范围宏（**禁止自造范围**）。

| CID | Key | Reg Start(0基址) | Reg Type | Type | Size | 范围宏(param.h) | Comment |
|---|---|---|---|---|---|---|---|
| 0 | TH | 0x0000 | HOLDING | U16 | 2 | 100~2000 | ×100 定点，1.0~20.0 |
| 1 | ALARM_CNT | 0x0001 | HOLDING | U16 | 2 | 1~100 | 连续异常帧→报警 |
| 2 | RELEASE_CNT | 0x0002 | HOLDING | U16 | 2 | 1~100 | 连续正常帧→解除 |
| 3 | WIN_LEN | 0x0003 | HOLDING | U16 | 2 | 固定 100 | 滑窗固定拒改 |
| 4 | WIN_ANOM | 0x0004 | HOLDING | U16 | 2 | 1~100 | 窗内异常帧阈值 |

所有项 `MB_DEVICE_ADDR1`（=0x01）、`PARAM_TYPE_U16`、`PARAM_SIZE_U16`、`PAR_PERMS_READ_WRITE`。

> **0 基址确认（R34/R35）**：官方字典 `mb_reg_start` 直接写 0 基址值（在册 §0 表，勿 +1）。主站发帧地址字段即 `mb_reg_start` 原样（预研 §1.4），返回即对应 §0 表首字。

---

## 4. 文件结构（骨架）

```
esp32-modbus-gateway/
├── CMakeLists.txt                # EXCLUDE_COMPONENTS freemodbus + project
├── sdkconfig.defaults
├── .gitignore
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml         # espressif/esp-modbus "^2"
│   ├── Kconfig.projbuild        # UART number/引脚/波特率 可配置项
│   ├── mb_param.h               # 数据字典 + 参数结构体（SoundDog 5×Holding）
│   └── master_app.c             # 主站 init + 轮询任务 + 串口打印
└── .reference/esp-modbus/       # 官方基准（只读参考，不参与编译）
```

---

## 5. 协商/待确认（本次实现前置）

- 无阻塞项：预研 §7（板型/引脚/上行/轮询周期）全部已由用户确认。
- ESP-IDF v5.4 已装 `C:\Espressif\v5.4`，满足 esp-modbus ^2 的 ≥5.0（预研 §6 项4 计时解决）。

---

## 6. 坑清单联动（本 FEAT 命中）

| 坑 | 落地 |
|---|---|
| GW-01/GW-08 | 0 基址字典 + 从站写路径整段校验（本期只读不写）|
| GW-02 | 0x02 异常=MB_ENOREG 越界，非链路断；轮询打印区分 |
| GW-10 | 115200-8N1 无校验 |
| GW-07 | UART HW RTS → GPIO19 接 DE/RE |
| GW-04 | 唯一轮询任务独占主站 API |
| GW-05 | 轮询周期 1000ms，控制 calloc 抖动 |