# 项目文档索引

> **单次会话只读 1 次**（`R1`）。做 X 前先查对应的 Y。修改本文档同步更新对应条目日期（`R2`）。
> 最后更新：2026-10-19（移植 embedspec `docs/INDEX.md`，按本项目实况重写；同日晚新增根 `README.md`）

## 场景检索地图

| 你要做什么 | 先读 |
|---|---|
| **改引脚 / UART / 波特率** | `main/Kconfig.projbuild`（`MB_UART_*`）＋ **`sdkconfig`（实际生效）** ＋ `docs/hardware/硬件配置-2026-09-22.md` |
| **核对板型 / 供电 / USB 口** | `docs/hardware/ESP32-S3-COREBOARD-V1.4-原理图.md`（`§5.1` 供电、`§5.2` USB-UART） |
| **给 ESP32 接独立电源 / 欠压 / 量 5V·3V3** | `docs/hardware/ESP32独立供电-选型接线与验收-2026-10-19.md`（**§3 先判 `J1.21` 是否悬空** → §5 串 100 Ω → §7 AC-1~7） |
| **改数据字典 / 寄存器映射** | `main/mb_param.h` ＋ `main/master_app.c`（描述符表）＋ `docs/spec/` |
| **改轮询 / 主站行为** | `main/master_app.c`（⚠️ `GW-04`：唯一任务独占主站 API） |
| **改 MQTT 上行 / 下行** | `main/wifi_mqtt.c` ＋ **必须同步 `tools/mqtt_payload.py`**（唯一事实源）＋ `main/Kconfig.projbuild`（topic） |
| **规划功能方向（新 FEAT）** | `docs/features/` 既有 `FEAT-ESPGW-01 ~ 05` |
| **实现具体功能（FEAT 5 阶段）** | 参照 `docs/features/FEAT-ESPGW-04`（整块读+堆诊断）/ `FEAT-ESPGW-05`（MQTT 下行）的 §2 决策记录（DR）＋ §5 AC ＋ §7 边界 |
| **编译 / 烧录失败** | `CLAUDE.md §构建环境`（本机唯一可用配方）＋ `docs/features/FEAT-ESPGW-04-…md §8`（四坑） |
| **调试崩栈 / 死机 / 复位循环** | `.workbuddy/memory/判据全集.md §一`（`T45`/`T48`/`T59`~`T62`）＋ `addr2line` 配方（`FEAT-ESPGW-04 §C`） |
| **查历史故障 / 现状** | `桌面\报告\审计报告\esp\ESP32-Modbus-Gateway-在办总表.md`（**单一事实源**）＋ `…\esp\检测记录\` |
| **Bug 登记** | `…\esp\检测记录\检测记录-YYYY-MM-DD.md`（`§6xx` 递增编号）——本项目未移植 `docs/bugs/`，见 §维护规则 |
| **出问题快速定位（文件→FEAT 反查）** | `.workbuddy/memory/判据全集.md §十三`（现状快照与待办）＋ `docs/features/` 各 FEAT 的 §3 实现步骤表 |
| **搭开发环境** | `CLAUDE.md §构建环境`（Git Bash 假通过 / `export.ps1` 大写 bug / `cmd` 被拦 / `cc1plus` OOM） |
| **调试器 / 烧录技能** | `.skills/flash.ps1`（编译+烧录+监视）｜`.skills/debug.ps1`（OpenOCD+GDB）｜`.skills/check_style.sh`（cppcheck） |

## 文档清单

| 文件 | 摘要 |
|---|---|
| `README.md` | **仓库门面**：定位/与 SoundDog 的关系、01~05 模块状态表、寄存器映射、读/写非对称设计、MQTT 线路协议、引脚占用、构建四坑、设计约束、已知遗留 |
| `CLAUDE.md` | 全局上下文：硬件/软件平台、固定宏、场景导航、构建环境配方、硬性边界 |
| `.claude/rules.md` | **AI 硬规则 R0~R32**（移植自 embedspec）＋ 本项目专属约束 P1~P7 ＋ 移植偏差登记 |
| `.skills/flash.ps1` | 一键编译＋烧录（＋`-Monitor`）；PowerShell 手工构造 IDF env |
| `.skills/debug.ps1` | OpenOCD（`idf.py openocd`）＋ `xtensa-esp32s3-elf-gdb`；退出必杀 openocd |
| `.skills/check_style.sh` | cppcheck 静态检查（`main/`） |
| `scripts/check_three_source.sh` | **R27 三源门禁**：新 API 调用必须有 ①源码 ②issue ③坑清单 证据 |
| `scripts/ci_local.sh` | 本地 CI 门禁（`R11`）：三源门禁 ＋ 单测；`--build` 追加构建 |
| `docs/features/FEAT-ESPGW-01-主站轮询-Modbus-RTU.md` | 主站骨架、数据字典、0 基址（GW-01）、裁剪决策 C-01~C-07 |
| `docs/features/FEAT-ESPGW-02-WiFi-MQTT-上行-实现与验证.md` | WiFi STA + MQTT 上行、断网重连、双出一源 |
| `docs/features/FEAT-ESPGW-02-WiFi-MQTT-上行-改动影响.md` | 02 的改动影响地图与风险对策 |
| `docs/features/FEAT-ESPGW-03-叠加SPI屏-LVGL-实现步骤.md` | 1.8″ TFT `ST7735S` + LVGL，坑清单 P-01~P-12 |
| `docs/features/FEAT-ESPGW-04-主站整块读与堆栈诊断-实现步骤.md` | 整块读（官方 #16）＋ 堆毒化/跟踪 ＋ 栈 8192；含本机 `idf.py` 四坑配方 |
| `docs/features/FEAT-ESPGW-05-MQTT下行控制-实现步骤.md` | MQTT 订阅 → 队列 → `set_parameter` → ack；K1~K8 坑清单 |
| `docs/hardware/ESP32-S3-COREBOARD-V1.4-原理图.md` | 本板原理图内部化（供电/USB/按键/strap/排针） |
| `docs/hardware/ESP32独立供电-选型接线与验收-2026-10-19.md` | **ESP32 独立 5V 供电操作卡**：选型（5V/2A 固定）＋ `J1.21` 悬空判据＋串 100 Ω＋共地＋AC-1~7 |
| `docs/hardware/硬件配置-2026-09-22.md` | 接线事实源（RS485 模块丝印、VCC=3.3V、⛔ 禁 5V 进总线） |
| `docs/hardware/从站底板-传感器+RS485-PCB设计规格-2026-10-19.md` | **2 层底板规格**：网络表/布局/走线/BOM/AC；含 M1~M5 **待实测**项与 F1~F4 翻车点 |
| `docs/spec/SoundDog-传感器扩展规格-SHT30-2026-10-19.md` | **从站交接规格**：SHT30 命令集（官方手册）＋寄存器 5→7 映射＋两侧同步硬约束＋S1~S8 坑清单 |
| `docs/spec/ESP32-Modbus-Gateway-三源预研.md` | 预研（从站事实、板型引脚、协议参数） |
| `docs/RS485抓信号台账-2026-09-22.md` | 抓信号记录 |
| `docs/RS485联调进度-2026-09-22.md` | 联调进度 |
| `tools/modbus_slave.py` | **从站语义真相**（5 寄存器 0..4，`WIN_LEN≡100` 拒写，范围校验） |
| `tools/mqtt_payload.py` | MQTT 上行/下行 payload 的 **Python 镜像**（与 C 端逐字符对撞，反查护栏） |
| `tools/test_*.py` | 单测（`pytest tools/ -q`，当前 **35 passed**） |

## 维护规则

1. **新增文档才补条目**；空目录、占位文件不录入。修改文档同步更新对应条目日期（`R2`）。
2. ⚠️ **本项目未移植** embedspec 的 `docs/bugs/`、`docs/troubleshooting/bug_shturl`、`docs/summary/lessons_summary.md`、`docs/features/*.template*`、`docs/features/维护地图.md`。
   **等价物映射（`R3`/`R5`/`R6`/`R15` 落到这两处）**：
   - 历史故障 / Bug 登记 → `桌面\报告\审计报告\esp\检测记录\检测记录-YYYY-MM-DD.md`
   - 经验教训 / 判据 → `.workbuddy/memory/判据全集.md`（§一 判据全集）
3. 🔴 **`docs/` 与当前代码冲突时，以代码 + `sdkconfig` + 实测为准**；冲突必须回填到对应 FEAT 文档并在本表标注日期（本项目 `docs/` 旧台账常带未结案结论，见 `T47`）。

---

*移植：审查官 ｜ 2026-10-19 14:4x*
