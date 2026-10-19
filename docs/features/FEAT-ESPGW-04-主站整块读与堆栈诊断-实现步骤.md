# FEAT-ESPGW-04：主站「一次读 5 寄存器」+ 堆/栈诊断（对位 §6bg 三源排错）

> 状态：**✅ 已实现 + 构建通过（EXIT=0）+ 改动已核入固件；⛔ 未烧录（待上电实测 AC-1~AC-7）**
> 日期：2026-10-16 21:4x
> 构建：`idf.py build` → `EXIT=0`，产物 `build/esp32_modbus_gateway.bin`（`0xee080` B，app 分区余 7%）
> 构建日志：`C:\Users\wumu2\AppData\Local\Temp\idf_build4.log`
> 动因：审查报告 `桌面\报告\审计报告\esp\检测记录\检测记录-2026-10-16.md §6bg`（三源排错）
> 依赖：FEAT-ESPGW-01（数据字典/轮询）、FEAT-ESPGW-02（MQTT 上行）
> 从站唯一事实源：`C:\Projects\SoundDog\firmware\soundDog\App\modbus_app.c`（多寄存器读已支持）
> 官方依据：`espressif/esp-modbus` issue **#135 / #164 / #16** ＋ releases（`v2.1.4`）

### 0.1 构建后自检（改动确实进了固件）

| 检查项 | 结果 |
|---|---|
| 新字典名 `SD_HOLD_ALL` | ✅ 1 处 |
| 新日志 `block read fail` / `block read implausible` / `discard+retry` | ✅ 各 1 处 |
| esp-modbus `2.1.4` 标志 `Master apply cooldown time`（EPROT-76） | ✅ 1 处 |
| 堆诊断（poison / heap trace / watchpoint）相关串 | ✅ 25 处 |
| `git status` | `M dependencies.lock, main/idf_component.yml, main/master_app.c, main/mb_param.h, sdkconfig.defaults` ＋ 本文件（未跟踪） |

> ⚠️ `sdkconfig` 被 `.gitignore` 排除，**不出现在 `git status`** ⇒ 改动前已手工备份（`T57`）。

---

## 0. 目标与范围

本 FEAT **只做两件事**，不改变既有硬件接线与打印口径：

| # | 目标 | 对位的病 |
|---|---|---|
| **GT-1** | 数据字典由「**5 个 CID 各读 1 个寄存器**」改为「**1 个 CID 一次读 5 个寄存器**」（`01 03 00 00 00 05`），并加**应用层自校验 + 失败清 FIFO 重读** | 「值整体前移一格 / `TH=0` / `0x108`」 |
| **GT-2** | 打开**堆毒化 + 堆跟踪 + 栈尾观察点**，轮询栈 `6144 → 8192` | 「`block_trim_free` ⇒ 约 3 s 崩溃复位循环」 |

**顺带**：升级 `esp-modbus 2.1.3 → 2.1.4`；打开 RX/TX **字节级**日志以坐实「回声帧」。

**不在范围**：⛔ 不改接线；⛔ 不改从站；⛔ **不烧录**（现场操作，须用户在场授权）。

### 0.2 ⛔ 待用户做的一步（唯一剩余动作）

```bat
cd /d C:\Projects\esp32-modbus-gateway
idf.py -p COM10 flash monitor
```

⚠️ 打开 `COM10` 会**复位 ESP32**（`T48`），要观测稳态请**保持端口开着**。
⚠️ 若不想先烧：`idf.py -p COM10 flash` 只烧不监视。
⚠️ **首轮只看三件事**：① 每轮是否只 1 条 `MB_TRANSMIT: 01 03 00 00 00 05 …` ② 有几条 `MB_RECV:`（**若某条字节 == 请求帧 ⇒ 「回声」坐实**）③ 是否还有 `block_trim_free` / `Rebooting...`。

---

## 1. 来源（可溯回链）

| 结论 | 出处 |
|---|---|
| `block_trim_free` = TLSF **堆损坏**；发现点在 WiFi 任务 `wifi_malloc` ⇒ **`T45` 发现点 ≠ 肇因点** | `检测记录 §6bf-B/C` |
| **一请求收到两帧**且两帧都通过 CRC/地址/功能码校验，`invoke handler` 被调用两次 | `检测记录 §6bg-E/F`（本地抓包 `com10_2119.bin` 重算） |
| **半双工【不做内部回环】**（`rs485tx_rx_en = 0`）⇒ 作废"TX 回环"成因 | `esp-idf/components/hal/esp32s3/include/hal/uart_ll.h:743-753` |
| **`event.trans_id` 在"取事件时"盖当前值** ⇒ 跨事务丢弃分支形同虚设 ⇒ 迟到应答被本事务吃下 | `port_event.c:96-97/133` ＋ `mb_master.c:601/625-629` |
| **`reg_buffer_ptr` 请求结束后不清空 ⇒ 悬垂**（use-after-free） | `mbc_serial_master.c:173/370`；官方 **#164** |
| 官方 **#16**：RTU 响应帧**无事务号、无起始地址** ⇒ 「读寄存器移位」**协议层无解**；建议**一次读整块 + 出错清 FIFO 重试 + 应用层自校验** | `espressif/esp-modbus#16`（Espressif 回复） |
| 官方 **#135**：同形堆损坏（崩在 `get_parameter` 的 `calloc`），修复进 **v2.1.1** | `espressif/esp-modbus#135` |
| `v2.1.4（2026-10-01）` 含 `EPROT-83 半处理状态事务处理` / `EPROT-76 rx 超时冷却` / `EPROT-69 广播帧处理` | esp-modbus releases |

---

## 2. 关键参数（决策记录）

| DR | 决策 | 理由 |
|---|---|---|
| **DR-1** | 字典缩到 **1 个 CID**，`mb_size = MASTER_PARAM_CNT(5)`，`param_size = sizeof(sounddog_holding_t)(10)` | ⚠️ `mbc_master_set_param_data()` 以 **`param_size` 字节**为准逐 `u16` 拷贝（`esp_modbus_master.c:468-472`）；若仍写 `PARAM_SIZE_U16(2)` ⇒ **只拷 2 字节，后 4 字段恒为 0** |
| **DR-2** | 保留 `MASTER_PARAM_CNT = 5`（寄存器个数），新增 `MASTER_CID_CNT = 1`（CID 个数） | 避免语义混淆；`device_parameters[]` 长度改用 `MASTER_CID_CNT` |
| **DR-3** | 打印**仍按 `CID#n 字段名 = 值` 逐字段输出** | 保持与历史日志（用户诊断口径）一致，避免改判读习惯 |
| **DR-4** | 校验用**从站自身语义**做整块判定（`WIN_LEN ≡ 100`、`TH∈[100,2000]`、其余 `[1,100]`） | 官方 #16 建议③；回声帧 `byte count = 0` ⇒ 全 0 ⇒ 必然被拦下 |
| **DR-5** | 失败/可疑 ⇒ `uart_flush_input(MB_PORT_NUM)` 后**重读一次**，两轮都不通过才跳过上行 | 官方 #16 建议②（ESP32 侧 `vMBMasterRxFlush()` 的等价物） |
| **DR-6** | 日志用 `esp_log_level_set("*", ESP_LOG_DEBUG)`，**不改 `CONFIG_LOG_DEFAULT_LEVEL`** | `MB_PRT_BUF` 用的是**动态 tag**（`parent_name + ":MB_RECV"`，`mb_common.h:71`），固定 tag 的两行 `esp_log_level_set` **覆盖不到** ⇒ 这正是历史抓包里 `:MB_RECV`/`:MB_TRANSMIT`/`:PORT_SEND` **全为 0 次**的原因 |
| **DR-7** | 堆诊断三项 + 栈扩容，均标为**诊断期临时** | 毒化/跟踪有开销；抓到第一现场后须关（§6 回滚） |

---

## 3. 实现步骤（已执行）

| 步 | 文件 | 改动 |
|---|---|---|
| **S1** | `main/idf_component.yml` | `version: "^2"` → `"~2.1.4"`；随后 `idf.py update-dependencies` |
| **S2** | `main/mb_param.h` | 新增 `MASTER_CID_CNT(1)`、`SD_HOLD_ALL_BYTES`；`extern device_parameters[MASTER_CID_CNT]` |
| **S3** | `main/master_app.c` | 字典表 5 条 → **1 条整块描述符**（CID 0） |
| **S4** | `main/master_app.c` | 新增 `master_snapshot_plausible()` + 字段名表 `s_field_name[]` |
| **S5** | `main/master_app.c` | 轮询任务：单次读 + 最多一次「清 FIFO 重读」；**保留 `CID#n NAME = v` 打印**；仅整块校验通过才 `gateway_mqtt_publish()` |
| **S6** | `main/master_app.c` | `mbc_master_set_descriptor(..., MASTER_CID_CNT)`；栈 `6144 → 8192` |
| **S7** | `main/master_app.c` | `app_main()` 加 `esp_log_level_set("*", ESP_LOG_DEBUG)` |
| **S8** | `sdkconfig` / `sdkconfig.defaults` | `HEAP_POISONING_COMPREHENSIVE=y`、`HEAP_TRACING_STANDALONE=y`、`FREERTOS_WATCHPOINT_END_OF_STACK=y` |

---

## 4. 关键代码骨架

```c
/* 1) 整块描述符（⚠️ DataSize 必须是字节数，不是 PARAM_SIZE_U16） */
const mb_parameter_descriptor_t device_parameters[MASTER_CID_CNT] = {
    { 0, STR("SD_HOLD_ALL"), STR("-"), SD_DEVICE_ADDR, MB_PARAM_HOLDING,
      HOLD_REG_START(th),        MASTER_PARAM_CNT,      /* 0x0000, 5 个寄存器 */
      HOLD_OFFSET(th),           PARAM_TYPE_U16, SD_HOLD_ALL_BYTES,   /* &holding_regs.th, 10 B */
      OPTS( 0, 0, 0 ), PAR_PERMS_READ },
};

/* 2) 应用层自校验（官方 #16 建议③） */
static bool master_snapshot_plausible(const sounddog_holding_t *p)
{
    return p
        && (p->win_len == SD_PARAM_WIN_LEN_FIX)                      /* 从站固定 100，拒写 */
        && (p->th >= SD_PARAM_TH_MIN_x100) && (p->th <= SD_PARAM_TH_MAX_x100)
        && (p->alarm_cnt   >= SD_PARAM_FRAME_MIN) && (p->alarm_cnt   <= SD_PARAM_FRAME_MAX)
        && (p->release_cnt >= SD_PARAM_FRAME_MIN) && (p->release_cnt <= SD_PARAM_FRAME_MAX)
        && (p->win_anom    >= SD_PARAM_FRAME_MIN) && (p->win_anom    <= SD_PARAM_FRAME_MAX);
}

/* 3) 一轮 = 一次请求 + 最多一次「清 FIFO 重读」 */
for (int attempt = 0; (attempt < 2) && !round_ok; attempt++) {
    if (attempt > 0) { (void)uart_flush_input(MB_PORT_NUM); }
    void *inst = master_get_param_data(&device_parameters[0]);
    esp_err_t err = mbc_master_get_parameter(master_handle, 0, (uint8_t *)inst, &type);
    if (err != ESP_OK) { /* warn */ continue; }
    if (!master_snapshot_plausible(&holding_regs)) { /* warn: discard+retry */ continue; }
    round_ok = true;
}
```

---

## 5. 验收清单（AC）

| # | 判据（上电后看 `COM10` 日志） | 期望 |
|---|---|---|
| **AC-1** | 每轮只出现 **1 次** `EV_FRAME_SENT`（不再 5 次） | 通过 |
| **AC-2** | 出现 `mbm_rtu@0x…:MB_TRANSMIT: 01 03 00 00 00 05 xx xx` | 一帧 8 B，长度字段 = `00 05` |
| **AC-3** | 出现 `mbm_rtu@0x…:MB_RECV: …` | ⭐ **若看到两条 `MB_RECV`，且其中一条字节 == 请求帧 `01 03 00 00 00 05 xx xx` ⇒ 「回声」坐实** |
| **AC-4** | 打印 `CID#0 TH` … `CID#4 WIN_ANOM` 五条，**值互不错位**（`WIN_LEN` 恒 `100`） | 通过 |
| **AC-5** | 不再出现 `assert failed: block_trim_free` / `Rebooting...` | 通过；⛔ 若仍崩 ⇒ 见 §7 |
| **AC-6** | 若仍崩，应出现 `CORRUPT HEAP: … Expected 0xfefefefe got 0x…` | ⇒ **坐实 use-after-free（#164）**，把 `got` 的值与地址记入检测记录 |
| **AC-7** | 从站 `COM4` 每轮只 1 条 `RX[8]` | 通过（请求数降为 1/5） |

---

## 6. 回滚（一条路，三处）

| 处 | 做法 |
|---|---|
| **代码/配置（git 跟踪）** | `git checkout -- main/ sdkconfig sdkconfig.defaults`（基线 `a79ccc6`，改动前工作区干净） |
| **`managed_components/`（不被 .gitignore 追，但 git 不跟踪）** | 手工备份在 `C:\Users\wumu2\AppData\Local\Temp\esp32gw_pre_FEAT04_20261016\managed_components`（2.1.3）⇒ 整目录拷回 |
| **`sdkconfig`（⚠️ 被 `.gitignore` 排除，git 回退**覆盖不到**）** | 同上备份目录里的 `sdkconfig` 原文件（`T57`：这类文件必须先手工存档） |

只想关诊断（保留整块读）：把 `sdkconfig` / `sdkconfig.defaults` 里改回
`HEAP_POISONING_DISABLED=y` / `HEAP_TRACING_OFF=y` / `FREERTOS_WATCHPOINT_END_OF_STACK=n`，
并删掉 `app_main()` 里的 `esp_log_level_set("*", ESP_LOG_DEBUG)`。

---

## 7. 绝对不动 / 待实测 / 边界

**⛔ 绝对不动**
- 接线（`A`/`B` 已对调、`RTS=GPIO10`）；从站固件；`uart_set_mode(UART_MODE_RS485_HALF_DUPLEX)`。
- ⛔ **不做**"调低 BOD / 换件 / 加电容"来处理本崩溃 —— 本窗口 `Brownout` = 0。

**⚠️ 待实测**
1. **「回声」仍为推断**（不是 AC-3 坐实不了就改结论）。
2. **`v2.1.4` 是否真修了 #164** —— 本地核对：`reg_buffer_ptr` **仍未清空**（`mbc_serial_master.c:173` 只赋值）⇒ ⚠️ **悬垂仍在**；2.1.4 新增的是 `curr_trans_id = 0` 复位（`mb_master.c:703`）与冷却（EPROT-76）⇒ **迟到帧在"报错后"才会被真丢弃**。
3. `reg_buffer_ptr` 只越界 2 字节（回调有长度防护）⇒ **是否足以致堆损坏未坐实**；**栈溢出候选 A 仍在册**（`T45`）。

**⚠️ 边界**
- `uart_flush_input()` 在轮询任务里调用，与 Modbus 端口任务存在**理论竞态**；仅在"上一事务已结束"时调用（本实现满足）。若出现异常，改为只在 `attempt>0` 前调用（已如此）。
- `esp_log_level_set("*", DEBUG)` 会显著抬高日志量 ⇒ 与"栈溢出"候选同向；已把栈加至 8192。**抓到第一现场后必须关掉**。

---

## 8. ⭐ 环境备忘：本机怎么真跑 `idf.py`（三处坑）

| 坑 | 现象 | 解法 |
|---|---|---|
| **① Git Bash 假通过** | `tools/idf.py` 见 `MSYSTEM` 就直接退出（退 0） ⇒ 看着成功其实没跑 | ⛔ 别用 Git Bash |
| **② `export.ps1` 炸** | `activate.py` 把 shell 识别成 `"powershell.EXE"`（大写）≠ 名单里的 `powershell.exe` ⇒ `Activation script failed` | ⛔ 别用 `export.ps1` |
| **③ cmd.exe 被拦** | PowerShell 工具里 `cmd.exe` 被安全策略拦；且 MSYSTEM 在进程级注入、`set "MSYSTEM="` 也清不掉 ⇒ `export.bat` 报 "for Windows CMD.EXE shell only" | ⛔ 别走 cmd |

| **④ `cc1plus.exe: out of memory`** | 默认并行度下 `nvs_flash/*.cpp`、`newlib/abort.c` 等 8 个目标报 OOM（**宿主内存不足，不是代码错**） | `idf.py build **-j 4**`（已验证可过） |

✅ **可用配方（PowerShell，手工构造环境）** —— 已验证 `idf.py --version` → `ESP-IDF v5.4`：

```powershell
$env:IDF_PATH='C:\Espressif\v5.4\esp-idf'
$env:IDF_TOOLS_PATH='C:\Users\wumu2\.espressif'
$t="$env:IDF_TOOLS_PATH\tools"; $py="$env:IDF_TOOLS_PATH\python_env\idf5.4_py3.13_env\Scripts"
$env:IDF_PYTHON_ENV_PATH="$env:IDF_TOOLS_PATH\python_env\idf5.4_py3.13_env"
# ⭐ cmake/ninja 必须【前置】（否则 STM32CubeCLT 的 cmake 抢 PATH，这是本项目四个坑之一）
$env:PATH="$t\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin;$t\esp32ulp-elf\2.38_20240113\esp32ulp-elf\bin;$t\cmake\3.30.2\bin;$t\ninja\1.12.1;$t\ccache\4.10.2\ccache\bin;$t\esp-rom-elfs\20241011;$py;$env:PATH"
Set-Location 'C:\Projects\esp32-modbus-gateway'
& "$py\python.exe" "$env:IDF_PATH\tools\idf.py" build
```

*记录：审查官 ｜ 2026-10-16 21:4x（按用户「都做」指令实现；⛔ 未烧录）*
