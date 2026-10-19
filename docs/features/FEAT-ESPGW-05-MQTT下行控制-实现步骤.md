# FEAT-ESPGW-05：MQTT 下行控制（远程写从站寄存器）—— 计划书 + 实现步骤

> 状态：**✅ 已实现 + 构建通过（EXIT=0）+ 单测 35 passed；⛔ 未烧录（AC-1~9 待上电）**
> 日期：2026-10-19 14:1x（14:2x 补：源③ 改坑清单、K6/K7 实际踩坑入表）
> 构建：`idf.py reconfigure` → `idf.py build` → **EXIT=0**，`bin = 0xeecf0`（app 分区余 7%）
> 测试：`tools/.venv/Scripts/python.exe -m pytest tools/ -q` → **35 passed**
> 动因：毕设要求「具有**远程监测和控制**功能」；现状只有上行（`esp_mqtt_client_publish`），**无下行通道、无 `set_parameter` 调用、无 subscribe**。
> 上游：FEAT-ESPGW-02（WiFi+MQTT 上行）、FEAT-ESPGW-04（整块读 + 堆/栈诊断）
> 规则源：`桌面\agent\embedspec`（`CLAUDE.md` / `.claude/rules.md` R0~R32 / `scripts/check_three_source.sh`）
> ⚠️ 边界：**从站 SoundDog 跨项目只读** ⇒ 传感器扩展（S2）只出规格书，⛔ 不改其源码。

---

## 0. 目标与范围

| # | 目标 | 不含 |
|---|---|---|
| **GT-1** | 网关**订阅** `cmd` topic，解析 `{"cid":N,"value":V}`，写从站保持寄存器 | ⛔ 不做 APP/Web 前端 |
| **GT-2** | 写结果**回执**上行 `ack` topic | ⛔ 不做鉴权/加密（`mqtt://` 明文，局域网演示） |
| **GT-3** | 守 `GW-04`：**主站 API 仍只由 `master_poll_task` 调用** | ⛔ 不改寄存器个数（传感器扩展等从站） |

### 0.1 规则合规检查表（本项目 AI 规则 = `.workbuddy/memory/判据全集.md`）

| 规则 | 出处 | 本设计如何遵守 |
|---|---|---|
| **GW-01** 字典 **0 基址**，禁止 +1 | `§九` | 描述符 `RegStart` 直接用 `HOLD_REG_START(field)`，不 +1 |
| **GW-04** 唯一轮询任务独占主站 API | `§九`、`MB_LOAD` | ⭐ MQTT 回调**只入队**（`xQueueOverwrite`），`set_parameter` 在 `master_poll_task` 里执行 |
| **GW-05** publish 非阻塞、断线丢弃 | `FEAT-02 §4` | 沿用 `esp_mqtt_client_publish` 异步 ＋ `CONFIG_MQTT_SKIP_PUBLISH_IF_DISCONNECTED=y` |
| **§九 payload 唯一事实源** ＋ **必须同步 `tools/mqtt_payload.py`** | `§九` | ⭐ 新增 ack payload ⇒ **同步加镜像 + 反查测试护栏** |
| **§九 `esp_event` 分支与注册必须成对** | `§九` | MQTT 用 `esp_mqtt_client_register_event(..., ESP_EVENT_ANY_ID, ...)` ⇒ `MQTT_EVENT_DATA` 天然覆盖 |
| **§七 引 `managed_components/`，不引 `.reference/`** | `§七` | 全部行号取自 `managed_components/espressif__esp-modbus` |
| **§十 不擅自 commit** | `§十` | 只改工作区，**不 commit** |
| **`T31` 可比性** | 判据 | 本轮只加新通道，**不碰** `FEAT-04` 已改的整块读/堆诊断四项 |

---

## 1. 来源（三源）

### 源① 本地官方源码（可复核）

| 结论 | 出处 |
|---|---|
| ⭐⭐ **分片判据是 `current_data_offset`/`total_data_len`，**⛔ 不是 `msg_id`** | `mqtt_client.h:190-200`（`msg_id` = "MQTT **messaged id** of message"）；`:69-73`（"only **first** event contains topic pointer and length"） |
| `MQTT_EVENT_DATA` 也可能一次到达完整报文 | `docs/en/api-reference/protocols/mqtt.rst:201` |
| ⭐ **`esp_mqtt_event_t` 的 `data` 无 NUL 结尾**，必须按 `data_len` 转存 | `mqtt_client.h:191-192` |
| 写 API 签名 | `esp_modbus_master.h:400`：`mbc_master_set_parameter(void *ctx, uint16_t cid, uint8_t *value, uint8_t *type)` |
| ⭐⭐ **写路径 `param_size` 语义与读相反**：`calloc(mb_size<<1)` 后按 **`param_size` 字节**从实例拷入 ⇒ 单寄存器写必须给 **2** | `mbc_serial_master.c:435-470` |
| ⭐ **字典允许"读整块 + 写单条"重叠**：`set_descriptor` 只校验 `cid == 序号`、`param_key != NULL`、`mb_size > 0` | `mbc_serial_master.c:134-153` |
| 从站整批写**原子**，`WIN_LEN` 固定拒写 | `SoundDog\...\App\modbus_app.c:100-116`；`tools\modbus_slave.py` |

### 源② 官方文档（本地 ESP-IDF）

`docs/en/api-reference/protocols/mqtt.rst:201` —— "For data that exceeds the internal buffer, **multiple MQTT_EVENT_DATA events** are posted and `current_data_offset` and `total_data_len` ... updated to keep track of the fragmented message." ⇒ 与源① 互证。

### 源③ 坑清单交付物（R27 口径：坑 → 后果 → 预防 → 验证判据）

> ⚠️ 修正：初稿把源③写成"权威社区"，那是外搜口径；本项目规则 `embedspec/scripts/check_three_source.sh` 明定
> **三源 = ①读被调函数源码 ②Issue 区调研 ③坑清单交付物**。本表即第③源。

| 坑 | 后果 | 预防 | 验证判据 |
|---|---|---|---|
| **K1** 用 `msg_id == 0` 判"完整报文" | 所有入站命令被误当分片丢弃，**功能完全不工作** | 分片判据只用 `current_data_offset == 0 && total_data_len == data_len` | AC-2：发一条命令必须打出 `cmd recv` |
| **K2** 以为写路径 `param_size` 同读（给 10） | `set_param_data` 只从实例拷 **10 字节**，越界读相邻字段 ⇒ **写出去的是垃圾** | 写描述符 `DataSize = PARAM_SIZE_U16(2)` | AC-7：写后读回值 == 下发值 |
| **K3** 整块写 5 个寄存器 | 从站整批写原子 + `WIN_LEN` 拒写 ⇒ **全批失败、一个都写不进** | 写用逐字段单寄存器 CID | AC-2/AC-4 |
| **K4** MQTT 回调里直调主站 API | 违反 `GW-04`，与轮询任务并发 ⇒ 事务号错乱（正是 `§6bg` 的病根） | 回调**只入队**，`set_parameter` 只在 `poll_task` 调 | 代码走查 + 压测：连续发 10 条命令不出现跨事务丢弃 |
| **K5** ack 缓冲也叫 `payload` | `test_payload_fmt_matches_c_source` 反查到 ack 那条 ⇒ **既有护栏误红** | 缓冲命名 `ack` | AC-8：`pytest tools/ -q` 全绿 |
| **K6** 注释里写出 `snprintf(payload, sizeof(payload),` 原文 | 同上，正则匹配注释 ⇒ 护栏误红（**本次实际踩到**） | 注释不抄调用原文 | AC-8 |
| **K7** 改 `Kconfig.projbuild` 后直接 `build` | `sdkconfig`/`build/config/sdkconfig.h` 无新符号 ⇒ `CONFIG_MB_MQTT_TOPIC_CMD undeclared`（**本次实际踩到**） | 改 Kconfig 后必须 `idf.py reconfigure` | AC-1：编译通过 |
| **K8** 订阅未在 `CONNECTED` 里重做 | 断线重连后订阅丢失 ⇒ 命令静默不响应 | 订阅写在 `MQTT_EVENT_CONNECTED` 分支内（每次重连都执行） | 拔网 30 s 再恢复，AC-2 仍通过 |

### 源④ 权威社区（辅助，非门槛）

本条暂缺：源①/源② 已闭环且本地可复核。若后续行为不符，再查 `esp-idf` issue。

---

## 2. 关键参数（决策记录）

| DR | 决策 | 理由 |
|---|---|---|
| **DR-1** | 字典 **1 条整块读（CID 0, 5 寄存器）+ 4 条单寄存器写（CID 1~4）** | 读必须整块（官方 #16：RTU 无事务号 ⇒ 逐条读会错位）；写**不能**整块（从站整批原子 + `WIN_LEN` 拒写 ⇒ 整块写必全批失败） |
| **DR-2** | 写描述符 `DataSize = PARAM_SIZE_U16(2)`；读描述符 `DataSize = 10` | 两个方向的 `param_size` 语义相反（见源①） |
| **DR-3** | 线路协议 = **纯文本 `{"cid":N,"value":V}`**，⛔ 不引 JSON 库 | 零新依赖；`cJSON` 会多 ~10 KB 且本项目无先例 |
| **DR-4** | 寄存器 **3（WIN_LEN）无写 CID**，本地直接拒 | 从站固定 100 拒写；本地拒可省一次注定失败的往返 |
| **DR-5** | 命令队列**单深度 + `xQueueOverwrite`** | 只关心最新命令；避免断线期积压导致"连上后连写一串" |
| **DR-6** | 写命令在**本轮读事务结束之后**执行 | 不打断进行中的读事务；仍是同一任务（GW-04） |
| **DR-7** | 未注册回调 = **纯上行模式**（不订阅） | 与 FEAT-02 行为完全兼容，便于 A/B 对比 |

### 2.1 ⭐ 一处**自我修正**（编码中发现，遵 `T46`）

初稿曾用 `event->msg_id == 0` 判"完整报文"。**源①/源② 均证伪**：`msg_id` 是 MQTT 报文 id（通常非 0），不是分片标志。
⇒ 改为 `event->current_data_offset == 0 && event->total_data_len == event->data_len && event->topic_len > 0`。

---

## 3. 实现步骤

| 步 | 文件 | 改动 |
|---|---|---|
| **S1** | `main/mb_param.h` | `MASTER_CID_CNT` 1→**5**；新增 `CID_READ_ALL`/`CID_WR_TH`/`CID_WR_ALARM_CNT`/`CID_WR_RELEASE_CNT`/`CID_WR_WIN_ANOM`/`CID_WR_NONE` |
| **S2** | `main/Kconfig.projbuild` | 新增 `MB_MQTT_TOPIC_CMD`（`esp32gw/sounddog/cmd`）、`MB_MQTT_TOPIC_ACK`（`esp32gw/sounddog/ack`） |
| **S3** | `main/wifi_mqtt.h` | `gateway_cmd_t`、`gateway_cmd_cb_t`、`gateway_mqtt_register_cmd_handler()`、`gateway_mqtt_publish_ack()` |
| **S4** | `main/wifi_mqtt.c` | `MQTT_EVENT_CONNECTED` 订阅；`MQTT_EVENT_DATA` 分支（**修正分片判据**）；`json_get_u16()`；ack 发布 |
| **S5** | `main/master_app.c` | 字典 1→5 条；`s_reg_to_wr_cid[]`；`master_apply_cmd()`；`master_on_mqtt_cmd()`（只入队）；轮询末尾 drain；`app_main` 建队列＋注册回调（**先于** `wifi_mqtt_init`） |
| **S6** | `tools/mqtt_payload.py` ＋ `tools/test_mqtt_payload.py` | ⭐ **§九 强制同步**：加 ack payload 镜像 + 反查护栏 |
| **S7** | — | 构建验证 `EXIT=0` + 固件字符串自检 |

---

## 4. 线路协议

```
下行  esp32gw/sounddog/cmd   {"cid":0,"value":650}
      cid = 寄存器号 0..4（0=TH 1=ALARM_CNT 2=RELEASE_CNT 3=WIN_LEN(拒) 4=WIN_ANOM）
回执  esp32gw/sounddog/ack   {"cid":0,"value":650,"err":0}
      err = esp_err_t 原值；0=成功；0x106 越界；0x107 超时；0x102=参数非法
上行  esp32gw/sounddog/holding  （FEAT-02 原样，未改格式）
```

---

## 5. 验收清单（AC）

| # | 判据 | 期望 |
|---|---|---|
| **AC-1** | 串口出现 `subscribed 'esp32gw/sounddog/cmd'` | 通过 |
| **AC-2** | 发 `{"cid":0,"value":650}` ⇒ 串口 `cmd recv: cid=0 value=650` ＋ `cmd applied: reg 0 (TH) = 650` | 通过 |
| **AC-3** | ack topic 收到 `{"cid":0,"value":650,"err":0}` | 通过 |
| **AC-4** | 发 `{"cid":3,...}`（WIN_LEN）⇒ `err=0x103`（`ESP_ERR_NOT_SUPPORTED`），**不产生任何总线事务** | 通过 |
| **AC-5** | 发 `{"cid":9,...}` ⇒ `err=0x102`（`ESP_ERR_INVALID_ARG`） | 通过 |
| **AC-6** | 发 `{"value":650}`（缺 cid）⇒ 串口 `cmd parse fail, drop` | 通过 |
| **AC-7** | 写后下一轮读回该字段 = 新值 | 通过 |
| **AC-8** | `pytest tools/ -q` 全绿（含 payload 反查护栏） | 通过 |
| **AC-9** | 断网时发命令 ⇒ publish 丢弃、轮询不受影响 | 通过 |

---

## 6. 回滚

| 处 | 做法 |
|---|---|
| 代码（git 跟踪） | `git checkout -- main/ tools/`（基线 `a79ccc6` + FEAT-04 的未提交改动 ⚠️ 见下） |
| ⚠️ **注意** | FEAT-04 的改动**尚未提交** ⇒ `git checkout -- main/` 会把 FEAT-04 一起回退。**要单独回退 FEAT-05** 必须手工删 5 处块（见 §3 表） |
| 配置 | `sdkconfig` 被 `.gitignore` 排除；备份在 `…\Temp\esp32gw_pre_FEAT04_20261016\` |

---

## 7. 待实测 / 边界

1. ⚠️ **broker 尚不存在**：`CONFIG_MB_WIFI_PASS=""`、`broker=mqtt://192.168.4.1:1883` ⇒ **上下行物理不通**。演示前必须配：PC 上跑 Mosquitto（`mqtt://<PC_IP>:1883`）或公共测试 broker。
2. ⚠️ **QoS**：订阅用 QoS 1；发布用 QoS 0（与 FEAT-02 一致）。QoS 1 下重复投递理论可能 ⇒ 写操作**幂等**（写同一值），可接受。
3. ⚠️ **无鉴权/无加密**（`mqtt://`）⇒ 仅限局域网演示，⛔ 不可外网暴露。
4. ⚠️ **`total_data_len` 超 128 B** 的报文会被截断解析失败（本项目报文 <64 B，可接受；超长即丢弃并告警）。
5. ⚠️ **寄存器 5→7 扩展未做**（等从站改造）。届时需同步改 `MASTER_PARAM_CNT` 与读描述符 `mb_size`，**否则整块读全挂（异常 0x02）**。

---

*记录：审查官 ｜ 2026-10-19 14:1x*
