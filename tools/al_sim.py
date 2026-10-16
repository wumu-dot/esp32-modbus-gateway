# SPDX-License-Identifier: Apache-2.0
"""
al_sim.py -- SoundDog Modbus 保持寄存器文件(5×Holding)语义的 Python 忠实建模。

设计总纲（FEAT-ESPGW-01 / tools/VALIDATION.md）：
  主站轮询到的 5 个保持寄存器读回的是 SoundDog「运行期可调参数」(debounce_param_t),
  不是状态机实时计数器。本模块提供：
    (1) DebounceSim.holding_regs()  —— 读路径快照, 与 modbus_app.c L86-91 逐行对齐;
    (2) DebounceSim.process()       —— 完整保留 debounce.c 双轨判决状态机(忠实参照),
                                       但明确【其计数不对外暴露】(仅内部判决用);
    (3) 写路径范围校验规则在从站层(modbus_slave.py)执行: 值越界→异常 0x03, WIN_LEN 拒写。

逐行对齐源（C -> Python）：
  - 状态机: DB_NORMAL --(轨1 连续>=alarm | 轨2 滑窗>=win_anom)--> DB_ALARMED
            DB_ALARMED --(连续正常>=release)--> DB_NORMAL
  - 计数清零: 正常态任一正常帧清轨1; 报警态任一异常帧清 解除累计
  - 轨2 滑窗: 100 bit 位图环形（模拟 C 每帧入一弹一, O(1) 维护窗内异常数）
  - 报警态内不再评估触发（防双轨重复）; 解除时清空滑窗防陈旧残留
  - 运行期参数可调（对应 debounce_set_params; WIN_LEN 固定 100 拒改）

源参照: C:/Projects/SoundDog/firmware/soundDog/BSP/debounce.c (+ debounce.h)
  与 C:/.../App/modbus_app.c (寄存器快照读路径)
  函数名/注释尽量保留 C 原貌, 便于 diff 与评审对应。
"""

from __future__ import annotations
from dataclasses import dataclass

# ---- 常量（逐字来自 debounce.h, 禁止自造） ----
DB_THRESHOLD = 6.0      # d_min 异常阈值
DB_CONSEC_ANOM_ALARM = 30  # 轨1: 连续异常帧 -> 报警
DB_CONSEC_NORM_RELEASE = 60  # 解除: 连续正常帧 -> 解除
DB_WIN_LEN = 100        # 轨2: 滑窗长度 (1s @100帧/s)
DB_WIN_ANOM_CNT = 30    # 轨2: 窗内异常帧 >= 此值 -> 报警
DB_COUNT_CAP = 60000    # 计数封顶

# ---- 状态 ----
DB_NORMAL = 0
DB_ALARMED = 1

# ---- 报警来源 ----
DB_SRC_NONE = 0
DB_SRC_CONSEC = 1
DB_SRC_MOFN = 2


@dataclass
class DebounceParam:
    """debounce_param_t (debounce.h:L49-54)。WIN_LEN 不入结构体(固定100)。"""
    threshold: float = DB_THRESHOLD
    consec_anom_alarm: int = DB_CONSEC_ANOM_ALARM
    consec_norm_release: int = DB_CONSEC_NORM_RELEASE
    win_anom_cnt: int = DB_WIN_ANOM_CNT


class DebounceSim:
    """debounce.c 状态机实例化。默认参数=宏, 可 set_params 在线覆盖。"""

    def __init__(self) -> None:
        self._param = DebounceParam()
        self._reset_state()

    # -- 内部状态复位（静态初始化=0, 对应 C 上电/看门狗自动清零, 坑4） --
    def _reset_state(self) -> None:
        self._state = DB_NORMAL
        self._consec_anom = 0  # 轨1
        self._consec_norm = 0  # 解除累计
        # 轨2: 100 bit 位图环形缓冲（python int 充当位图, 或 list[bit]; 用 list 对齐 C 的字位数组语义）
        self._win_bits: list[int] = [0] * DB_WIN_LEN
        self._win_anom = 0
        self._win_head = 0      # 0..99 环形

    # -- 运行期参数（debounce_get_param / debounce_set_params 语义） --
    def get_param(self) -> DebounceParam:
        # C: 临界区拷贝返回副本; python 无并发撕裂, dataclass 天然副本
        return DebounceParam(**{f.name: getattr(self._param, f.name)
                                for f in DebounceParam.__dataclass_fields__.values()})

    def set_param(self, p: DebounceParam | None) -> None:
        if p is None:
            return
        self._param = p

    # -- 轨2 位图窗口（对齐 debounce.c db_win_push / db_bitmap_*) --
    def _db_win_push(self, anomaly: bool) -> None:
        old = self._win_bits[self._win_head]
        if old:
            self._win_anom -= 1
        self._win_bits[self._win_head] = 1 if anomaly else 0
        if anomaly:
            self._win_anom += 1
        self._win_head = (self._win_head + 1) % DB_WIN_LEN

    def _db_bitmap_clear(self) -> None:
        self._win_bits = [0] * DB_WIN_LEN
        self._win_head = 0

    # -- 主接口: 逐帧判决（对齐 debounce_process） --
    def process(self, frame_is_anomaly: bool) -> dict:
        """喂一帧(是否异常), 返回 {changed,state,src,count}。对齐 C 出参。"""
        changed = 0
        src = DB_SRC_NONE
        count = 0

        if self._state == DB_NORMAL:
            # 轨1: 连续异常计数（任一正常帧清零 -> 连续语义）
            if frame_is_anomaly:
                if self._consec_anom < DB_COUNT_CAP:
                    self._consec_anom += 1
            else:
                self._consec_anom = 0
            # 轨2: 每帧都入窗
            self._db_win_push(frame_is_anomaly)

            # 双轨 OR 判据（先到先报; 同帧双满足 -> 轨1 优先）
            if (self._consec_anom >= self._param.consec_anom_alarm or
                    self._win_anom >= self._param.win_anom_cnt):
                if self._consec_anom >= self._param.consec_anom_alarm:
                    src = DB_SRC_CONSEC
                    count = self._consec_anom
                else:
                    src = DB_SRC_MOFN
                    count = self._win_anom
                self._state = DB_ALARMED
                self._consec_anom = 0
                self._consec_norm = 0
                changed = 1

        else:  # DB_ALARMED: 只评估解除判据（不重复触发）
            if not frame_is_anomaly:
                if self._consec_norm < DB_COUNT_CAP:
                    self._consec_norm += 1
            else:
                self._consec_norm = 0
            # 滑窗仍维护, 但报警态内不用于触发
            self._db_win_push(frame_is_anomaly)

            if self._consec_norm >= self._param.consec_norm_release:
                src = DB_SRC_NONE
                count = self._consec_norm
                self._state = DB_NORMAL
                self._consec_norm = 0
                self._win_anom = 0
                self._db_bitmap_clear()
                changed = 1

        return {
            "changed": changed,
            "state": self._state,
            "src": src,
            "count": count,
        }

    # -- 寄存器快照（对齐 modbus_app.c 读路径 L86-91） --
    #
    # 【重要修正, FEAT-ESPGW-01】读回的是「运行期可调参数」(debounce_param_t),
    # 不是 debounce 状态机的实时计数器! 逐行对照 modbus_app.c:
    #   L87: holding[0] = p.threshold*100+0.5   (TH 参数)
    #   L88: holding[1] = p.consec_anom_alarm    (参数, 非 s_consec_anom)
    #   L89: holding[2] = p.consec_norm_release  (参数, 非 s_consec_norm)
    #   L90: holding[3] = DB_WIN_LEN             (固定100)
    #   L91: holding[4] = p.win_anom_cnt         (参数, 非 s_win_anom)
    # 因此 SoundDog 报警状态(DB_ALARMED/NORMAL)【不】通过 Modbus 暴露:
    # 输入/线圈/离散恒 MB_ENOREG(modbus_app.c:L131-150), 无报警状态寄存器。
    def holding_regs(self) -> list[int]:
        """返回 5 个保持寄存器当前值(读=参数)。帧0..4 与从站读路径一一对应。"""
        p = self._param
        th_x100 = int(p.threshold * 100.0 + 0.5)  # ×100 定点, 就近取整 (L87)
        return [
            th_x100,               # 0x0000 TH
            p.consec_anom_alarm,   # 0x0001 ALARM_CNT (参数)
            p.consec_norm_release, # 0x0002 RELEASE_CNT (参数)
            DB_WIN_LEN,            # 0x0003 WIN_LEN (固定100)
            p.win_anom_cnt,        # 0x0004 WIN_ANOM (参数)
        ]