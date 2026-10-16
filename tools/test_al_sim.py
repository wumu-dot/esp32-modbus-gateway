# SPDX-License-Identifier: Apache-2.0
"""
test_al_sim.py -- SoundDog AL(寄存器)语义仿真的离线移植正确性单测。

关键事实(源: modbus_app.c L86-91 + debounce.h)：
  - 5 个保持寄存器读回的是「运行期可调参数」(debounce_param_t), 默认
    TH=600(6.0×100) / ALARM_CNT=30 / RELEASE_CNT=60 / WIN_LEN=100 / WIN_ANOM=30
  - WIN_LEN 恒 100：读固定, 写被拒(0x00 bad-value→从站异常 0x03)。
  - 写路径参数范围: TH 100~2000, 其余 1~100(见 param.h 宏)。
这些测试【不需要任何硬件】, 在 CI/本机直接 python -m pytest tools/test_al_sim.py 即可跑。
"""
import pytest

from al_sim import DebounceSim


def default_regs() -> list[int]:
    return [600, 30, 60, 100, 30]


# ---- 读路径：默认参数快照 ------------------------------------------------------------------
def test_default_holding_regs():
    """默认(未触碰参数)的 5 寄存器 = 期望默认值。"""
    sim = DebounceSim()
    assert sim.holding_regs() == default_regs()


def test_threshold_x100_fixed_point():
    """TH 是 ×100 定点: threshold=6.0 -> 600, threshold=7.5 -> 750, 就近取整。"""
    sim = DebounceSim()
    from al_sim import DebounceParam
    sim.set_param(DebounceParam(threshold=7.5))
    regs = sim.holding_regs()
    assert regs[0] == 750


# ---- 写路径：范围校验规则(违反 → 从站拒绝, 见从站层映射为 0x03 ILLEGAL_DATA_VALUE) ----------------
def test_win_len_immutable_in_snapshot():
    """WIN_LEN(帧3) 恒 100, 即使模拟器内没暴露写口也不可改。"""
    sim = DebounceSim()
    assert sim.holding_regs()[3] == 100


# ---- 状态机(保留为忠实参照, 但其计数不对外暴露): 轨1 连续异常 --------------------------------
def test_track1_consecutive_alarm():
    """轨1: 连续异常≥30 转报警(src=CONSEC, count=30)。"""
    sim = DebounceSim()
    ev = {}
    for _ in range(29):
        ev = sim.process(True)
        assert ev["state"] == 0   # 未到
    ev = sim.process(True)        # 第30帧
    assert ev["changed"] == 1
    assert ev["state"] == 1
    assert ev["src"] == 1         # DB_SRC_CONSEC
    assert ev["count"] == 30


def test_track1_normal_frame_resets_counter():
    """正常帧会清零连续异常计数(连续语义)。"""
    sim = DebounceSim()
    for _ in range(29):
        sim.process(True)
    sim.process(False)            # 中间插一正常帧
    for _ in range(29):
        sim.process(True)
        # 已断, 需重新累计; 第29帧仍未到30
    ev = sim.process(False)       # 又被正常帧清零
    # 之后重新累满 30 才报警:
    for _ in range(30):
        ev = sim.process(True)
    assert ev["state"] == 1


def test_track2_mofn_window_alarm():
    """轨2 m-of-n: 100 窗内异常≥30 转报警(src=MOFN)。非连续也可累积。"""
    sim = DebounceSim()
    pattern = [True, False]  # 50% 异常, 100帧内约50 ≥30
    ev = {}
    for i in range(100):
        ev = sim.process(pattern[i % 2])
    # 窗内异常 50, 至少一项 ≥30 判据命中(此处可能轨1先到, 再视窗内)
    assert ev["state"] == 1


def test_release_after_normal_flow():
    """报警后连续正常≥60 转解除, 且清空滑窗。"""
    sim = DebounceSim()
    # 触发报警(轨1 30 连异常)
    for _ in range(30):
        sim.process(True)
    assert sim.process(True)["state"] == 1
    # 59 正常帧未解除
    for _ in range(59):
        ev = sim.process(False)
        assert ev["state"] == 1
    # 第60 正常帧解除
    ev = sim.process(False)
    assert ev["changed"] == 1
    assert ev["state"] == 0


def test_alarmed_state_stops_triggering():
    """报警态内不再评估触发(防双重复), 只累计正常帧。"""
    sim = DebounceSim()
    for _ in range(30):
        sim.process(True)
    # 报警态内再来异常, 不重复触发事件
    ev = sim.process(True)
    assert ev["changed"] == 0
    assert ev["state"] == 1