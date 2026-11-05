# 项目全局上下文（会话自动继承）
<!-- DOC-STATE: CHIP=ESP32-S3(N16R8), RTOS=FreeRTOS_ESP-IDF, BUILD=CMake+idf.py+xtensa-esp32s3-elf-gcc, TASKS=1业务任务(modbus_poll)+IDF内置, HSE=N/A(模组40MHz晶振), MIN_STACK=8192 -->
<!-- 最后同步：2026-10-19（FEAT-ESPGW-05 落地后） -->

> **规则全集** = `.claude/rules.md`（R0~R32，强制）｜**文档索引** = `docs/INDEX.md`（先读它，R1）
> **项目记忆** = `.workbuddy/memory/MEMORY.md`（索引，自动注入）＋ `判据全集.md`（全文，按需读）

## 硬件平台

| 项 | 值 |
|---|---|
| 主控 | **ESP32-S3-N16R8**（ESP32-S3-WROOM-1 模组，16 MB Flash + 8 MB **Octal** PSRAM） |
| 时钟源 | 模组 40 MHz 晶振（应用层无需配 HSE）；`CPU 240 MHz` |
| 调试接口 | **USB2 原生 USB（USB-Serial-JTAG，`COM10`，`303A:1001`）**；`COM9` = CH343P（`1A86:55D3`，带自动下载电路） |
| 对端 | **SoundDog（STM32F407ZGT6）Modbus RTU 从站**，addr `0x01`，RS485 半双工 |
| RS485 | UART2，`TXD=GPIO17`／`RXD=GPIO16`／`RTS=GPIO10(DE+~RE)`，**115200 8N1**（`sdkconfig` 为实际生效源） |

## 软件环境

| 项 | 值 |
|---|---|
| RTOS | **FreeRTOS（ESP-IDF v5.4 内置）**，业务任务仅 1 个：`modbus_poll`（栈 **8192**，prio 1，core 1） |
| 驱动库 | **ESP-IDF v5.4**（`C:\Espressif\v5.4\esp-idf`）＋ `espressif/esp-modbus` **2.1.4** |
| 编译构建 | **CMake + `idf.py` + xtensa-esp32s3-elf-gcc**（⚠️ 本机跑法见下"构建环境"） |
| 单测 | `tools/.venv/Scripts/python.exe -m pytest tools/ -q`（当前 **35 passed**） |

## 固定全局宏（禁止随意修改）

| 宏 | 值 | 语义 |
|---|---|---|
| `MASTER_PARAM_CNT` | `9u` | 从站保持寄存器个数（**两侧耦合**：从站 `MODBUS_REG_HOLDING_NUM` 必须一致）。`0~4` 参数 + `5~8` 只读遥测（温/湿/振动 RMS/峰值） |
| `MASTER_CID_CNT` | `5u` | 字典条目数 = 1 条整块读（CID 0）＋ 4 条单寄存器写（CID 1~4） |
| `CID_READ_ALL` / `CID_WR_*` | 0 / 1~4 | **读整块、写单条**——⛔ 不可整块写（从站整批写原子 + `WIN_LEN` 拒写） |
| `POLL_PERIOD_MS` | `1000u` | 轮询周期 |
| `SD_DEVICE_ADDR` | `0x01u` | 从站地址 |
| `GPIO10` | RTS/DE | ⚠️ `GPIO19/20` 被 USB2 占用、`GPIO43/44` 接 CH343P、`GPIO33~37` 被 Octal PSRAM 占用 ⇒ **均不可挪用** |

## 场景导航（做 X 前先读 Y）

| 你要做什么 | 先读这个 |
|---|---|
| **改引脚 / UART / 波特率** | `main/Kconfig.projbuild`（`MB_UART_*`）＋ **`sdkconfig`（实际生效）** ＋ `docs/hardware/硬件配置-2026-09-22.md` |
| **改数据字典 / 寄存器映射** | `main/mb_param.h` ＋ `main/master_app.c`（描述符表）＋ `docs/spec/` |
| **改轮询 / 主站行为** | `main/master_app.c`（⚠️ **`GW-04`：唯一任务独占主站 API**） |
| **改 MQTT 上行 / 下行** | `main/wifi_mqtt.c` ＋ **必须同步 `tools/mqtt_payload.py`**（§九 唯一事实源） |
| **改 SPI 屏 / LVGL** | `docs/features/FEAT-ESPGW-03-叠加SPI屏-LVGL-实现步骤.md`（坑清单 P-01~P-12） |
| **规划功能方向（新 FEAT）** | `docs/features/` 下既有 `FEAT-ESPGW-0N`（01~05） |
| **编译或烧录失败** | `.claude/rules.md §4` ＋ `docs/features/FEAT-ESPGW-04-…md §8`（**本机跑 `idf.py` 的可用配方**） |
| **调试崩栈 / 死机** | `.workbuddy/memory/判据全集.md §一`（`T45`/`T48`/`T59`~`T62`）＋ `addr2line` 配方（`FEAT-ESPGW-04 §C`） |
| **查历史故障/现状** | `桌面\报告\审计报告\esp\ESP32-Modbus-Gateway-在办总表.md`（**单一事实源**）＋ `…\esp\检测记录\` |
| **出问题快速定位** | `.workbuddy/memory/判据全集.md §十三`（现状快照与待办） |
| **Bug 登记** | 本项目暂用 `…\esp\检测记录\检测记录-YYYY-MM-DD.md`（`§6xx` 编号），见 `docs/INDEX.md` |
| **搭开发环境** | `docs/features/FEAT-ESPGW-04-…md §8`（⚠️ Git Bash 假通过 / `export.ps1` 大写 bug / `cmd` 被拦 / `cc1plus` OOM 四坑） |

## 开发硬性边界

1. 🔴 **`GW-04`**：`master_poll_task` 是**唯一**访问主站 API 的任务；其他任务/回调**只入队**，⛔ 不直调 `mbc_master_*`。
2. 🔴 **`GW-01`**：数据字典 **0 基址**，禁止 +1。
3. ⛔ **不改** `managed_components/`、`.reference/`、`build/`（R19）；⛔ **不擅自 `git commit`**（用户要求）。
4. ⛔ **不改从站 SoundDog 源码**（跨项目只读；需改时只出规格书，由执行方实施）。
5. ⚠️ **`sdkconfig` / `managed_components/` 被 `.gitignore` 排除** ⇒ git 回退覆盖不到，改动前**必须手工备份**（`T57` 已为此永久丢过 2 个文件）。

---

## 芯片预设（ESP32 系列 · 本项目采用）

| 字段 | 值 |
|---|---|
| 调试接口 | USB-Serial-JTAG（`COM10`）＋ UART0 经 CH343P（`COM9`） |
| RTOS | FreeRTOS（ESP-IDF 内置） |
| 驱动库 | ESP-IDF v5.4 |
| 编译构建 | CMake + `idf.py` + xtensa-esp32s3-elf-gcc |
| 固定宏 | `MASTER_PARAM_CNT=9`（**寄存器数**；`MASTER_CID_CNT=5` 不变——遥测项无写 CID）、`POLL_PERIOD_MS=1000`、`UART2 115200 8N1`、`RTS=GPIO10` |
| 硬性边界 | 不改 `components/`/`managed_components/` 下 framework 文件；WiFi/MQTT 栈任务优先级不修改 |

### 🔴 构建环境（本机唯一可用路径，`T48` 环境指纹）

Git Bash 里 `idf.py` **假通过**（见 `MSYSTEM` 就退 0）、`export.ps1` 因 `activate.py` 把 shell 识别成 `"powershell.EXE"` 而失败、`cmd.exe` 被安全策略拦 ⇒ **只能用 PowerShell 手工构造环境**：

```powershell
$env:IDF_PATH='C:\Espressif\v5.4\esp-idf'; $env:IDF_TOOLS_PATH='C:\Users\wumu2\.espressif'
$t="$env:IDF_TOOLS_PATH\tools"; $py="$env:IDF_TOOLS_PATH\python_env\idf5.4_py3.13_env\Scripts"
$env:IDF_PYTHON_ENV_PATH="$env:IDF_TOOLS_PATH\python_env\idf5.4_py3.13_env"
$env:CMAKE_BUILD_PARALLEL_LEVEL='4'     # ⭐ 否则 cc1plus OOM；idf.py build -j 4 无效
$env:PATH="$t\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin;$t\esp32ulp-elf\2.38_20240113\esp32ulp-elf\bin;$t\cmake\3.30.2\bin;$t\ninja\1.12.1;$t\ccache\4.10.2\ccache\bin;$t\esp-rom-elfs\20241011;$py;$env:PATH"
Set-Location 'C:\Projects\esp32-modbus-gateway'
& "$py\python.exe" "$env:IDF_PATH\tools\idf.py" build      # ⚠️ 改过 Kconfig 先 reconfigure
```

✅ 一键脚本：`.skills\flash.ps1`（编译＋烧录＋监视）
