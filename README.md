# ESP32 Modbus Gateway — 工业设备云端监测网关

> *"A gateway that never guesses."*

基于 **ESP32-S3** 的工业设备云端监测与远程控制网关：以 **Modbus RTU 主站**身份，经 **RS485 半双工**总线轮询 **SoundDog（STM32F407ZGT6）** 节点的 **9 个保持寄存器**（报警阈值/去抖参数 + SHT30 温湿度 + 预留振动），把设备状态以 **JSON** 按 **ThingsCloud 物模型**上报，同时接受云端下发的**参数写命令**。

这是**配套从站 [`SoundDog`](https://github.com/wumu-dot/SoundDog) 的上位网关侧**：从站负责声学异常判决（I2S + MFCC + 距离比对），网关负责把判决结果与运行参数送上云、把控制指令送回现场。

---

## 当前状态

| 模块 | 状态 | 说明 |
|------|------|------|
| **01 Modbus RTU 主站** | 🟢 **已实现** | UART2 `TXD=GPIO17` / `RXD=GPIO16` / `RTS=GPIO10(DE+~RE)`，115200-8N1，硬件 RTS 自动驱动半双工方向（`GW-07`）；轮询周期 1000 ms（`GW-05`） |
| **02 WiFi STA + MQTT 上行** | 🟢 **已实现** | STA 连热点，JSON 快照发布；断线周期重连；`CONFIG_MQTT_SKIP_PUBLISH_IF_DISCONNECTED=y` 不积压（`GW-05`） |
| **04 整块读 + 堆栈诊断** | 🟢 **已实现并上电验证** | **9 寄存器一次读满**（对位官方 issue #16）＋ 应用层快照校验 ＋ 读失败清 RX FIFO 重试一次；堆毒化/跟踪、栈 8192。✅ 已烧录 `8f63d923` |
| **05 MQTT 下行控制** | 🟢 **已实现并上电验证** | 订阅 `attributes/push` → 单深度队列 → 唯一轮询任务执行 `set_parameter`；**回执为本地日志**（ThingsCloud 无 ack 主题，擅发未声明键会 406）。单测 **39 passed** |
| **03 SPI 屏 + LVGL** | 🟡 **方案已立，未实施** | 1.8″ TFT `ST7735S` 本地可视化，`FEAT-ESPGW-03`；排在「固件稳定性收敛 + B→A 稳定」之后开工（坑清单 P-01~P-12 已备） |
| **RS485 方向 A→B**（主站发 → 从站收） | 🟢 **确证稳定** | 10-27 结案：80.2 s / 74 轮 + 清理探针后 19.3 s / 20 轮 + 新设备 86.3 s / 79 轮，合计 **232 轮全对**。现每轮发整块读 `01 03 00 00 00 09`，应答 **23 字节** |
| **RS485 方向 B→A**（从站发 → 主站收） | 🟢 **已闭环** | 10-27 连续 232 轮全对；10-29 扩到 9 寄存器后又跑 **4,842 轮 / 83.4 min**，`0x108`、超时、事务外丢帧**均为 0**。⚠️ 保留教训：`ESP_OK` ≠ 读对了——错位应答同样返回 `ESP_OK`，必须校验值域 |
| **两道硬坎（已排除）** | ✅ | ① 主站 `RS2` 与从站 `RS1` 是**不同厂家**模块，**TTL 丝印约定相反**：主站要**直连**（`TX→TXD`、`RX→RXD`），交叉只能收 0 字节（单变量前后对比：`0` 字节 vs `151 × 0x2E`）；② `A`/`B` **反相**，对调后乱码消失（位流模拟实证：`7F 7E FF FF FD…` = 原文逐位取反） |
| **主站固件稳定性** | 🟢 **已解决** | 10-27 起不再出现 `block_trim_free` 崩溃；83.4 min 长跑**无重启、无 `Guru Meditation`、无 `assert`**。历史根因记录保留：`tlsf_control_functions.h:548` 堆损坏（指向 esp-modbus #164 use-after-free，官方未修） |
| **从站存活** | 🟢 **在跑** | 10-29 实测：SWD 读到 `s_present=1`，温湿度刷新生效（吹气 24.1→29.9 ℃ 后回落）；Modbus 9 寄存器整块读 4,842 轮全对 |
| **MQTT 实机联通** | 🟢 **已验证（ThingsCloud）** | 10-29 22:36 全链路闭环：`WiFi got IP` → `MQTT connected` → `subscribed 'attributes/push'`，云端 **10 个属性全部有值**。⚠️ 83 min 长跑中 MQTT 在线率仅 **32.7%**，归因为**手机热点上行不可用**（WiFi 关联率 96%、23:00–00:00 时段 99.8%），非固件缺陷 |

> ⚠️ **状态口径**：`已实现 + 构建通过` ≠ `功能已验证`。后者判据是 FEAT 文档里的 AC 条目**逐条上电实测通过**（见 `.claude/rules.md` `R21` / `R25`）。
> **当前已有上电验收记录**（9 寄存器整块读 + ThingsCloud 全链路，见上表 04/05 与 MQTT 行）；但**振动通道（寄存器 7/8）恒为 0**——MPU6050 未接入，**不要据此认为"振动已实现"**。
>
> 🔴 **`T64` — 引用当前状态前必须回表**：唯一采信源是 `桌面\报告\审计报告\esp\ESP32-Modbus-Gateway-在办总表.md`。本文档的状态表若与该表冲突，**以该表为准**。历史上有过若干处「已作废却仍在流通」的表述（模组 `RXD`→`DI` 断线 / `A/B` 零跳变推断点 / 发送链已证清白 / 「RS485 已打通」），作废登记见该表 §14。

### 固件产物

| 项 | 值 |
|---|---|
| target | `esp32s3`（N16R8） |
| 构建 | `idf.py build` → EXIT=0（最后一次构建 **2026-10-29**） |
| bin | `build/esp32_modbus_gateway.bin` = **978,880 B**（`0xEEFC0`） |
| app 分区 | `factory` @ `0x010000`，大小 **1,048,576 B** ⇒ 余 **69,696 B（6.6%）**（分区表实解析，非估算） |
| 组件 | `espressif/esp-modbus` **2.1.4**（含 EPROT-83 / EPROT-76 / EPROT-69 三项修复） |
| ✅ 板上实际运行的固件 | **9 寄存器版**（2026-10-29 烧录，SHA-256 前缀 `8f63d923`），源码对应提交 **`761a63f`** |
| ⚠️ 别拿 bin hash 比版本 | ESP-IDF 在 `esp_app_desc_t` 里嵌入**编译时间** ⇒ 同源码每次重建 hash 都变。**判"是否同版本"要比 git commit，不比 hash** |

---

## 数据面

### 寄存器映射（0 基址，`GW-01`）

| 寄存器 | 字段 | 单位 / 范围 | 方向 |
|---|---|---|---|
| `0x0000` | `TH` 报警阈值 | ×100 定点，`100~2000`（1.0~20.0） | 读 / 写 |
| `0x0001` | `ALARM_CNT` 连续异常帧数 | `1~100` | 读 / 写 |
| `0x0002` | `RELEASE_CNT` 连续正常帧数 | `1~100` | 读 / 写 |
| `0x0003` | `WIN_LEN` 滑窗长度 | **固定 100，从站拒写** | 只读 |
| `0x0004` | `WIN_ANOM` 窗内异常阈值 | `1~100` | 读 / 写 |
| `0x0005` | `TEMP_C` 温度 | ×10 加偏移定点：`743` ⇒ **24.3 ℃**（换算 `(v−500)/10`） | 只读 |
| `0x0006` | `HUMI_RH` 湿度 | ×10 定点：`573` ⇒ **57.3 %RH**（换算 `v/10`） | 只读 |
| `0x0007` | `VIB_RMS` 振动 RMS | mg 定点，`0~5000` | 只读，**MPU6050 未接入 ⇒ 恒 0** |
| `0x0008` | `VIB_PEAK` 振动峰值 | mg 定点，`0~5000` | 只读，**MPU6050 未接入 ⇒ 恒 0** |

> 共 **9 个**寄存器：`0~4` 参数（4 个可写 + `WIN_LEN` 拒写）、`5~8` 只读遥测。
> ⚠️ `5~8` **没有写 CID**，`MASTER_CID_CNT` 仍为 5（1 整块读 + 4 单写）。

> 🔴 范围宏唯一事实源 = SoundDog 侧 `firmware/soundDog/BSP/param.h`，本项目**只引用不自造**。

### 读 / 写的非对称设计（刻意的）

| 操作 | 帧 | 原因 |
|---|---|---|
| **读** | CID 0，一次读满 **9 个**寄存器（`01 03 00 00 00 09`，应答 **23 B**） | Modbus RTU 响应帧**无事务号、无起始地址**（官方 issue #16）⇒ 逐寄存器读会"值整体前移一格" |
| **写** | CID 1~4，每次 1 个寄存器 | 从站整批写是**原子**的（任一越界 ⇒ 全批拒、保留旧值），且 `WIN_LEN` 拒写 ⇒ 整块写**必定全批失败** |

⚠️ `param_size` 在读路径与写路径语义相反：读给**字节数**（`9×2 = 18`），写给**单寄存器字节数**（2）。写错方向的后果是"后 4 个字段恒为 0"，且**不会报错**。

> ✅ **代码现状 = 板上现状**：板上跑的就是本仓库 `761a63f` 构建的 9 寄存器版（2026-10-29 烧录），帧形 `01 03 00 00 00 09` / 应答 23 B。⛔ 引用历史日志时注意：10-16 那批日志是**旧版 5-CID 固件**（每轮 5 个单寄存器请求，帧长 `00 01`），不要与现版本混引。

### MQTT 线路协议（ThingsCloud 物模型，纯文本拼装，无 JSON 库）

```jsonc
// 上行属性  →  attributes                     （QoS 0；限频 CONFIG_MB_MQTT_PUBLISH_PERIOD_S = 30 s）
{"th":600,"alarm_cnt":30,"release_cnt":60,"win_len":100,"win_anom":30,
 "temperature":24.3,"humidity":57.3,"vib_rms":0,"vib_peak":0,"ok":9}

// 下行属性  ←  attributes/push                （键 = 平台物模型属性标识符）
{"th":650}

// 兼容旧格式（自建 / 公共 broker）：{"cid":0,"value":650}
// 两种都能收；键→寄存器号的映射表见 wifi_mqtt.c 的 k_attr_map[]
//
// ⚠️ 回执不发布：ThingsCloud 没有 ack 主题，擅发 {"cid":..,"err":..} 会被 406 拒收
//    并可能断连 ⇒ 回执仅打本地日志，真正的回执由下一轮属性上报体现。
```

> 📌 **键名注意**：物模型用 `temperature` / `humidity`（平台自带单位与量程），而结构体成员仍叫 `temp` / `humi`（`mb_param.h`）——**JSON 键 ≠ 成员名**，这一例外已写进 `tools/mqtt_payload.py` docstring 与测试注释。
> 📌 `ok` 是本轮成功读取的寄存器个数（元数据），当前应为 **9**。

> 📌 这三个格式有 **Python 镜像**（`tools/mqtt_payload.py`），并由单测**反向从 C 源码里提取格式串**做逐字符对撞（`test_payload_fmt_matches_c_source`）。改 C 端不改镜像 ⇒ 单测必红。

---

## 硬件平台

- 主控：**ESP32-S3-N16R8**（ESP32-S3-WROOM-1，16 MB Flash + 8 MB **Octal** PSRAM），CPU 240 MHz，模组自带 40 MHz 晶振
- 收发器：**MAX3485**（3.3 V RS485，半双工）
- 供电：板载 `CJ6107A33GW` LDO（5 V → 3.3 V）；⚠️ **独立供电方案见 `docs/hardware/ESP32独立供电-选型接线与验收-2026-10-19.md`**
- 调试口：**USB2 原生 USB-Serial-JTAG**（`COM10`，VID:PID `303A:1001`）；`COM9` 是 CH343P（`1A86:55D3`），二者**不是同一个口**

### 引脚占用（改引脚前必读）

| GPIO | 用途 | 备注 |
|---|---|---|
| `GPIO16` | UART2 RXD ← MAX3485 `RO` | |
| `GPIO17` | UART2 TXD → MAX3485 `DI` | |
| `GPIO10` | UART2 RTS → MAX3485 `DE`＋`~RE` | 🔴 `GPIO19/20` 被 USB2 占、`GPIO43/44` 接 CH343P、`GPIO33~37` 被 Octal PSRAM 占 ⇒ **均不可挪用** |

---

## 软件环境

- RTOS：**FreeRTOS（ESP-IDF v5.4 内置）**，业务任务仅 **1 个** `modbus_poll`（栈 **8192**，prio 1，core 1）
- 框架：**ESP-IDF v5.4** ＋ `espressif/esp-modbus` 2.1.4（组件管理器）
- 构建：**CMake + `idf.py` + xtensa-esp32s3-elf-gcc**
- 单测：`tools/.venv/Scripts/python.exe -m pytest tools/ -q` → **39 passed**（2026-11-06 实跑）；依赖见 `tools/requirements.txt`（`pymodbus>=3.6`、`pytest>=8.0`）

### 构建（Windows 上有四个坑，详见 `CLAUDE.md §构建环境`）

在**已激活 ESP-IDF 环境**的终端里（`export.ps1` / VS Code ESP-IDF 插件 / 官方 Installer 均可）：

```powershell
$env:CMAKE_BUILD_PARALLEL_LEVEL='4'     # ⭐ Windows 上不加会 cc1plus OOM；idf.py build -j 4 无效
idf.py build                            # 改过 Kconfig 先 idf.py reconfigure
```

| 坑 | 现象 |
|---|---|
| Git Bash 里跑 `idf.py` | 见 `MSYSTEM` 直接退 **0** ⇒ **假绿**（最危险的一个） |
| 某些环境下的 `export.ps1` | `activate.py` 把 shell 识别成 `"powershell.EXE"`（大写）⇒ 失败 |
| 部分机器 `cmd.exe` 被策略拦 | `idf.py` 内部走 cmd 会断 |
| 默认并行编译 | `cc1plus.exe: out of memory` ⇒ 必须 `CMAKE_BUILD_PARALLEL_LEVEL=4` |

### 烧录与调试

```powershell
.\.skills\flash.ps1              # 编译 + 烧录 + 监视（COM10）
.\.skills\debug.ps1              # idf.py openocd + xtensa-esp32s3-elf-gdb
bash .\scripts\ci_local.sh       # 三源门禁 + pytest（--build 追加构建，Git Bash 下拒绝）
```

> 📌 烧录/监视**必须走 PowerShell**。Git Bash 里的 `idf.py` 会返回 0 但什么都没做。

---

## 目录结构

```
main/
  master_app.c        主站初始化 + 数据字典 + 轮询任务（⚠️ GW-04：唯一任务独占主站 API）
  mb_param.h          数据字典：9×Holding 结构体 + offsetof>>1 地址宏（GW-01 0 基址）
  wifi_mqtt.c/.h      WiFi STA + MQTT 上行/下行（订阅 attributes/push → 队列 → 本地 ack 日志）
  Kconfig.projbuild   全部可配项：UART 引脚/波特率、WiFi、MQTT topic
tools/
  modbus_slave.py     从站语义真相（pymodbus）——9 寄存器 0..8，WIN_LEN≡100 拒写
  mqtt_payload.py     MQTT payload 的 Python 镜像（与 C 端逐字符对撞）
  al_sim.py           算法仿真
  test_*.py           单测（8 + 13 + 5 + 13 = 39 项）
scripts/
  check_three_source.sh   R27 三源门禁：新 API 调用必须有 ①源码 ②issue ③坑清单
  ci_local.sh             R11 本地 CI：三源门禁 + pytest（--build 追加构建）
.skills/
  flash.ps1 / debug.ps1 / check_style.sh   一键烧录 / GDB 调试 / cppcheck
docs/
  INDEX.md            文档索引（场景检索地图，先读它）
  features/           FEAT-ESPGW-01 ~ 05（含决策记录 DR、验收 AC、坑清单）
  hardware/           原理图内部化、接线事实源、独立供电操作卡、从站底板 PCB 规格
  spec/               跨项目规格书（从站传感器扩展等）
CLAUDE.md             项目全局上下文（会话自动继承）
.claude/rules.md      AI 硬规则 R0~R32（三源法 / 证据门闩 / 影响图 / 风险分级）
```

> ⚠️ **`gateway_arduino/` 已废弃**（2026-09-22 裁决：现役 = ESP-IDF）。保留仅为历史对照，**不参与构建**，勿在此继续开发。

---

## 设计约束（改代码前必读）

| 编号 | 约束 |
|---|---|
| **`GW-01`** | 数据字典 **0 基址**，禁止 +1 |
| **`GW-04`** | `master_poll_task` 是**唯一**访问主站 API 的任务；其他任务/回调**只入队**，⛔ 不直调 `mbc_master_*` |
| **`GW-07`** | RS485 半双工方向由 **UART 硬件 RTS** 自动驱动 |
| **`GW-10`** | 串口参数 **115200-8N1 无校验**，`0x107` 超时先查链路/波特率 |
| **`R21`/`R25`** | 声称"完成/通过"必须贴证据；功能验证必须**扰动输入并观察输出跟随** |
| **`R27`** | 新增 API 调用前必须三源核实（源码 / issue / 坑清单），`scripts/check_three_source.sh` 会拦 |
| **`R19`** | ⛔ 不改 `managed_components/`、`.reference/`、`build/` |
| — | ⛔ 不改从站 SoundDog 源码（跨项目只读，需改只出规格书） |
| ⚠️ | `sdkconfig`、`managed_components/` 被 `.gitignore` 排除 ⇒ **git 回退覆盖不到**，改动前手工备份 |

---

## 文档导航

新读者建议顺序：`docs/INDEX.md` → `CLAUDE.md` → `docs/features/FEAT-ESPGW-01` → `FEAT-ESPGW-04` / `05`。

- 想改**引脚/UART** → `main/Kconfig.projbuild` + `sdkconfig`（实际生效）+ `docs/hardware/硬件配置-2026-09-22.md`
- 想改**寄存器/字典** → `main/mb_param.h` + `main/master_app.c` + `docs/spec/`
- 想改 **MQTT** → `main/wifi_mqtt.c` + **必须同步** `tools/mqtt_payload.py`
- 遇到**构建/烧录失败** → `CLAUDE.md §构建环境` + `FEAT-ESPGW-04 §8`
- 遇到**崩栈/复位循环** → `.workbuddy/memory/判据全集.md §一`（`T45`/`T48`/`T59`~`T62`/`T64`）
- 想确认**当前到底什么状态** → `桌面\报告\审计报告\esp\ESP32-Modbus-Gateway-在办总表.md`（**单一事实源**，`T64`：与本文冲突时以它为准）

---

## 已知遗留

> ✅ **10-27 起三条历史拦路虎均已闭环**：`block_trim_free` 崩溃循环、B→A 方向未稳定、FEAT-04/05 未烧录。详细证据见 `在办总表` 与 `RS485与云端闭环复核报告-2026-10-27`。以下只列**当前仍存在**的问题。

- 🔴 **振动通道未接入**（`VIB_RMS` / `VIB_PEAK` 恒 0）：MPU6050 未实施，方案见 SoundDog 侧 `FEAT-A7-04` / `FEAT-A7-05`。⚠️ **不要据寄存器存在就认为"已支持振动"**
- 🔴 **无故障工况验证**：所有数据均来自"静止/轻微扰动"，**从未在真实旋转设备上测过"故障 vs 正常"**。融合效果（单源 vs 多源）因此无法给出对比结论
- 🟡 **云端在线率仅 32.7%**（83 min 长跑）：归因为测试所用**手机热点上行不可用**（WiFi 关联率 96%、23:00–00:00 时段 99.8%），非固件缺陷；需换稳定 AP 重测才能给出可信的连通率
- 🟡 **`RS2` 与 `RS1` 是不同厂家模块，TTL 丝印约定相反**：主站直连、从站交叉。⛔ 别把任一侧的结论外推到另一侧（`T56`）。且 `RS2` 卖家资料**已被撤回**（先称"TX 接 TX"，后承认"资料搞错"）⇒ **"直连"只由实测支撑，不靠厂商文档**
- 🟡 **`sdkconfig` 与 `managed_components/` 被 `.gitignore` 排除** ⇒ git 回退覆盖不到，改动前必须手工备份（历史上因此永久丢过 2 个文件）
- 🟡 **`esp_log_level_set("*", ESP_LOG_DEBUG)` 是诊断期开关**：忘关会明显抬高日志量并占栈——当前为 `INFO`
- 🟡 **FEAT-03（SPI 屏）未实施**，方案与 12 条坑清单已就绪

---

## 配套仓库

| 仓库 | 角色 |
|---|---|
| [`SoundDog`](https://github.com/wumu-dot/SoundDog) | **从站**：STM32F407ZGT6，INMP441 + I2S3 + MFCC 声学异常判决，FreeModbus RTU 从站，addr `0x01` |

---

*本仓库文档与结论均以「审查官」署名；每一条状态判断都附可复核的证据出处或"待实测"标记。*
