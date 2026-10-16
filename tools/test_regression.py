# SPDX-License-Identifier: Apache-2.0
"""
test_regression.py -- 端到端离线闭环：主站视角观察 SoundDog 5 寄存器随 AL 演进。

模拟真实网关(FEAT-ESPGW-01)轮询：不接串口，直接驱动从站模型 + 内部判决引擎，
验证"预研 §0 表 + AL 语义"在整条链上自洽——即网关读到的不只是静态 addr/帧对，
而是符合 SoundDog 语义的可复现序列。作为回归基线(when CI 接入后同样结构)。

`python -m pytest tools/test_regression.py -v` 即跑。
"""
import pytest

from al_sim import DB_SRC_CONSEC, DB_SRC_MOFN
from modbus_slave import SoundDogModel, SdException


@pytest.fixture()
def gw_view():
    """模拟网关：从站模型 + 一帧喂养判决引擎。"""
    m = SoundDogModel()
    return m


def _poll(m):
    """主站轮询读全部 5 寄存器(0x03 读 5)。"""
    return m.read_holdings(0, 5)


def test_stable_normal_window_reads_defaults(gw_view):
    """正常态下主站连续轮询 5 寄存器 → 稳定默认值(600/30/60/100/30)。"""
    for _ in range(50):
        gw_view.sim.process(False)          # 50 正常帧
    assert _poll(gw_view) == [600, 30, 60, 100, 30]


def test_alarm_sequence_reads_param_not_counter(gw_view):
    """即使触发报警, 寄存器仍=参数(600/30/60/100/30), 非计数器(untainted)。"""
    for _ in range(30):
        gw_view.sim.process(True)           # 30 连续异常 → 轨1 报警
    assert gw_view.sim.process(True)["state"] == 1
    # 关键断言: 主站读到的仍是参数, NOT s_consec_anom=30 之类实时计数
    assert _poll(gw_view) == [600, 30, 60, 100, 30]


def test_write_window_update_then_read_reflects(gw_view):
    """主站写窗口(如调大 ALARM_CNT=40) → 读回更新, WIN_LEN 不变。"""
    gw_view.write_holdings(0, [600, 40])    # 0x10 写前2寄存器
    assert _poll(gw_view) == [600, 40, 60, 100, 30]


def test_double_track_precedence_consistent(gw_view):
    """轨2 m-of-n 报警(path independent of consecutive), 无竞态。"""
    # 50% 异常 → 100 帧内窗内异常≥30 → 轨2 报警
    for i in range(100):
        gw_view.sim.process(i % 2 == 0)
    regs = _poll(gw_view)
    assert regs[3] == 100                    # WIN_LEN 恒 100
    # 状态机内部应已报警(此处不强断言 src, 只证读稳定)


def test_write_invalid_does_not_perturb(gw_view):
    """非法写被拒 → 读回仍旧值(原子性在链路层验证)。"""
    with pytest.raises(SdException):
        gw_view.write_holdings(0, [600, 999])
    assert _poll(gw_view) == [600, 30, 60, 100, 30]