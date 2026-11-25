# esp32-modbus-gateway 重构方案（规则与文档治理）

- **日期**：2026-11-26
- **出具**：审查官
- **状态**：🟡 **P0/P1 已执行（2026-11-26）**（P0 `782a0e9`／P1 `1b7a8df`）；P2 未开始
- **边界**：**只重构"规则与文档治理"，不动 ESP-IDF 代码、不动 Modbus 字典、不动云端物模型。**
- **母版**：`桌面\agent\embedspec`（template，已修 10 项悬空引用）

---

## 0. 现状实测

| 指标 | 值 | 对比 SoundDog |
|---|---:|---|
| `.claude/rules.md` | **12,147 B** | 24,873 B（本项目更瘦） |
| 规则号 | 31 个（R0~R32 段） | 34 个 |
| `CLAUDE.md` | 6,399 B，**含 `DOC-STATE` 行** | 同（6,577 B） |
| **pre-commit** | **❌ 无** | ✅ 有（2 个 PowerShell） |
| `scripts/` | 4 个 | 24 个 |
| `docs/` | `features` / `hardware` / `spec` | 8 个子目录 |

### 三个真问题

| # | 问题 | 证据 |
|---|---|---|
| **1** | 🔴 **`check_three_source.sh` 从未被触发** | 脚本存在（59 行，实现完整），但 **`.git/hooks/pre-commit` 不存在** ⇒ R27 三源门禁是**死代码**。这是全项目最贵的一处空转 |
| **2** | **`DOC-STATE` 行存在但无校验器** | `CLAUDE.md:2` 有 `<!-- DOC-STATE: CHIP=ESP32-S3(N16R8), RTOS=…, TASKS=… -->`；但 `scripts/` 下**没有** `check-doc-drift` 类脚本 ⇒ 这行会随代码变化静默过期 |
| **3** | **跨仓库重复文档** | `docs/spec/` 里放着 **2 份 SoundDog 的规格**：`SoundDog-传感器扩展规格-MPU6050-2026-10-20.md`、`SoundDog-传感器扩展规格-SHT30-2026-10-19.md` ⇒ **同一事实两个仓库各存一份**，必然漂移 |

---

## 1. 目标（可判定）

| # | 目标 | 判据 |
|---|---|---|
| G1 | 门禁真正生效 | pre-commit 存在，且至少跑 `check_three_source.sh` |
| G2 | DOC-STATE 可校验 | 有脚本能把 `CLAUDE.md` 的 DOC-STATE 与 `sdkconfig`/`CMakeLists` 比对；不一致能报 |
| G3 | 单一事实源 | `docs/spec/` 不再持有从站规格（迁回 SoundDog 或改为一行指针） |

**明确不做**：不改 `main/` 代码、不改 `sdkconfig`、不动 `docs/features` 的 FEAT 正文。

---

## 2. 方案（三阶段）

### P0 装 pre-commit（预计 20 分钟）—— 收益最高

```sh
#!/bin/sh
bash scripts/check_three_source.sh || exit 1      # 已存在，只是没接线
python scripts/check_rule_refs.py  || exit 1      # P1 产物，先留占位
```

| 动作 | 细节 |
|---|---|
| 建 `.git/hooks/pre-commit` | 两行，`chmod +x` |
| 验证**真的会拦** | 造一个"引入新 API 但无三源证据"的改动 → 提交应被拒（**必须实测，不能假设**） |
| 记录 | 钩子不入库，在 `README` 写清重建命令（换机后一条命令恢复） |

> ⚠️ 参照 SoundDog 的教训：**钩子装完必须用负例验证**。2026-11-25 日报钩子第一版因 `core.quotepath` 转义中文名而**静默放行**，看起来装了其实没装。

### P1 补 DOC-STATE 校验（预计 1.5 小时）

新增 `scripts/check-doc-drift.py`，检 4 个 ESP32 特有字段：

| 字段 | 取值来源 | 校验方式 |
|---|---|---|
| `CHIP` | `sdkconfig` 的 `CONFIG_IDF_TARGET` | 字符串比对 |
| `FLASH` | `CONFIG_ESPTOOLPY_FLASHSIZE` | 比对（N16R8 ⇒ 16MB） |
| `PSRAM` | `CONFIG_SPIRAM` | 必须 = y（N16R8 有 8MB PSRAM） |
| `IDF` | `idf.py --version` 或用 `dependencies.lock` | 比对主次版本 |

**先 WARN 后 FAIL**（理由同 SoundDog：误报一次就会被绕过）。

### P2 处理跨仓重复（预计 40 分钟）

| 选项 | 做法 | 评价 |
|---|---|---|
| **A（推荐）** | 两份 SoundDog 规格**移到 `SoundDog/docs/hardware/`**，gateway 的 `docs/spec/` 只留一行指针 | 符合"单一事实源"；代价是 gateway 开发者要多跳一个仓库 |
| B | 留在 gateway，SoundDog 侧改为指针 | 与"规格属于从站"的归属相悖 |
| C | 两边都留，加"以哪边为准"的声明 | ❌ 不做——这正是漂移的起点 |

---

## 3. 风险与止损

| 风险 | 后果 | 止损 |
|---|---|---|
| 钩子误拦正常提交 | 团队改用 `--no-verify` | P0 只挂 `check_three_source.sh`（已在其他项目验证过），不一次挂多个 |
| 钩子静默放行 | 以为有保护 | **P0 必须做负例测试**（造违规 → 应拒） |
| DOC-STATE 字段与 sdkconfig 命名不一致 | 校验器本身错 | 先打印两边实际值，人眼确认一次再自动化 |
| 迁文档导致链接失效 | 文档断链 | 迁移后全库 `grep` 旧路径，逐个改 |

---

## 4. 验收清单

| # | 条目 | 判据 |
|---|---|---|
| AC-1 | 钩子存在且可执行 | `test -x .git/hooks/pre-commit` |
| AC-2 | **钩子真的会拦** | 负例提交被拒（贴输出） |
| AC-3 | DOC-STATE 校验可跑 | 故意改错 `TASKS` → 脚本报不一致 |
| AC-4 | 跨仓文档已处理 | `docs/spec/` 下无 SoundDog 规格全文（只剩指针） |
| AC-5 | 不破坏构建 | `idf.py build` 仍 EXIT=0 |
| AC-6 | 无新增目录 | 只新增 1 个 `.py` + 改 1 个钩子 |

---

## 5. 与 SoundDog 方案的差异（为什么不能照搬）

| 项 | SoundDog | gateway |
|---|---|---|
| 瓶颈 | 规则**太长**（24.9 KB） | 门禁**没接线**（有脚本无钩子） |
| 第一优先 | 外置 §7 | **装 pre-commit** |
| 校验器 | 规则引用校验 | DOC-STATE 校验 + 规则引用校验（共用） |
| 文档问题 | 无跨仓重复 | **有 2 份** |

⇒ **同一套母版，两个项目的第一刀切在不同地方。** 照搬会砍错。

---

*出具：审查官 ｜ 2026-11-26 ｜ 未执行，等确认*

---

## 6. 执行记录

### P0 装 pre-commit（2026-11-26，`782a0e9`）

| 项 | 结果 |
|---|---|
| 钩子安装 | `.git/hooks/pre-commit` 存在且可执行，跑 `check_three_source.sh` |
| 钩子源码入库 | `scripts/git-hooks/pre-commit`（3 行），换机一条命令重建 |
| **负例实测** | 造 `main/_hooktest.c` 加 `esp_wifi_start()` 无三源标记 ⇒ **提交被拒**，HEAD 停在 `cae4e72` |
| **正例实测** | 同文件补「三源」注释 ⇒ **放行**，生成 `a374bdf` |
| 测试清理 | `git reset --hard cae4e72` + 删文件 ⇒ 工作区 **0 改动**，无残留 |
| 活体验证 | 紧接着的真实提交（README 改动）触发钩子并正确跳过（⊘ 无 .c/.h 变更） |

**意义**：`check_three_source.sh` 从"存在但从不运行"变成"每次提交都跑"。这是本项目此前最贵的一处空转。

**遗留（P1 待做）**：DOC-STATE 校验器未建；`docs/spec/` 里 2 份 SoundDog 规格未处理。

### P1 建 DOC-STATE 校验（2026-11-26，`1b7a8df`）

| 项 | 结果 |
|---|---|
| 新增 `scripts/check_doc_drift.py` | 只做能可靠判定的：**CHIP 归一化比对**（`ESP32-S3(N16R8)` ↔ `CONFIG_IDF_TARGET="esp32s3"`） |
| 未能校验的字段 | `FLASH` / `PSRAM`：**DOC-STATE 里根本没有这两个字段** ⇒ 只打印 sdkconfig 实际值（16MB / y / OCT=y）并 WARN 建议补充，**不 FAIL**（加字段是文档决策，不该由脚本替人决定） |
| 显式声明未覆盖 | `RTOS` / `BUILD` / `TASKS` / `HSE` / `MIN_STACK` 逐项打印并标注「本版不校验」，避免误以为已覆盖 |
| 负例验证 | 把 CHIP 改成 `ESP32-C3` → 归一化 `esp32c3 vs esp32s3` 报不一致、`--fail` 退出 1；备份还原后 md5 一致、退出码回 0 |

**P1 遗留**：未接进 pre-commit（属 P2）；`docs/spec/` 的 2 份 SoundDog 规格未处理（属 P2）。
