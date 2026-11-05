# SPDX-License-Identifier: Apache-2.0
"""
test_slave.py -- SoundDog 从站模型核心读写语义回归（零依赖、离线）。

覆盖 FEAT-ESPGW-01 §3 数据字典: 0 基址 + 范围校验 + WIN_LEN 拒写 + 原子写。
纯逻辑, `python -m pytest tools/test_slave.py` 即跑, 无需真板/串口。
"""
import pytest

from modbus_slave import SoundDogModel, SdException


@pytest.fixture()
def slave():
    return SoundDogModel()


# ---- 读 0x03：0 基址 + 参数快照 ----
def test_read_defaults_param_snapshot(slave):
    """读=参数快照(600/30/60/100/30)，非状态机计数器。"""
    assert slave.read_holdings(0, 5) == [600, 30, 60, 100, 30]


def test_read_zero_based(slave):
    """0 基址：帧3(WIN_LEN)=100 应落在地址3。"""
    assert slave.read_holdings(3, 1) == [100]
    assert slave.read_holdings(4, 1) == [30]


def test_read_out_of_range_addr(slave):
    """读越界地址 → 异常 0x02。"""
    with pytest.raises(SdException):
        slave.read_holdings(9, 1)
    with pytest.raises(SdException):
        slave.read_holdings(8, 2)   # 跨出 9 边界


# ---- 5..8 只读遥测（2026-10-24 从站由 5 扩到 9） ----
def test_read_telemetry_slice(slave):
    """遥测 5..8 默认 500/0/0/0：温度存的是 ×10+500 偏移（500 ⇒ 0.0℃）。"""
    assert slave.read_holdings(5, 4) == [500, 0, 0, 0]


def test_read_telemetry_reflects_injected_values(slave):
    """注入遥测后读回应实时反映（743 ⇒ 24.3℃、573 ⇒ 57.3%RH）。"""
    slave.temp_x10 = 243      # 243+500 = 743
    slave.humi_x10 = 573
    slave.vib_rms = 128
    slave.vib_peak = 256
    assert slave.read_holdings(5, 4) == [743, 573, 128, 256]


def test_telemetry_write_rejected(slave):
    """写 5..8 → 异常 0x02（从站 reg_off_to_pid 落 default ⇒ MB_ENOREG），且不得改动参数。"""
    with pytest.raises(SdException) as exc:
        slave.write_holdings(5, [600])
    assert exc.value.code == 0x02
    assert slave.read_holdings(0, 5) == [600, 30, 60, 100, 30]   # 参数未被波及


# ---- 写 0x10：合法写 → 参数生效 → 读回更新 ----
def test_write_valid_th_updates(slave):
    slave.write_holdings(0, [1250])          # TH×100 = 12.5
    assert slave.read_holdings(0, 1) == [1250]


def test_write_multiple_valid(slave):
    slave.write_holdings(0, [600, 40, 55, 100, 25])
    assert slave.read_holdings(0, 5) == [600, 40, 55, 100, 25]


# ---- 写范围校验：越界拒、原子、WIN_LEN 拒写 ----
def test_write_th_out_of_range_rejected(slave):
    with pytest.raises(SdException) as e:
        slave.write_holdings(0, [50])        # <100 非法
    assert e.value.code == 0x03              # ILLEGAL_DATA_VALUE(值越界)
    assert slave.read_holdings(0, 5) == [600, 30, 60, 100, 30]


def test_write_batch_atomic_on_any_invalid(slave):
    """整批任一非法 → 整批拒绝(全保留旧值)。"""
    with pytest.raises(SdException) as e:
        slave.write_holdings(0, [600, 30, 60, 100, 999])  # 最后项 999>100
    assert e.value.code == 0x03              # ILLEGAL_DATA_VALUE(值越界)
    assert slave.read_holdings(0, 5) == [600, 30, 60, 100, 30]


def test_write_win_len_rejected(slave):
    """WIN_LEN 恒 100：写其它值拒，写 100 合法(不变)。"""
    with pytest.raises(SdException) as e:
        slave.write_holdings(3, [50])
    assert e.value.code == 0x03              # WIN_LEN 固定,写入它值=ILLEGAL_DATA_VALUE
    slave.write_holdings(3, [100])
    assert slave.read_holdings(3, 1) == [100]


def test_write_out_of_range_addr(slave):
    with pytest.raises(SdException):
        slave.write_holdings(5, [100])


def test_write_other_frames_not_affect_win_len(slave):
    slave.write_holdings(1, [40])
    assert slave.read_holdings(1, 1) == [40]
    assert slave.read_holdings(3, 1) == [100]   # 不受影响