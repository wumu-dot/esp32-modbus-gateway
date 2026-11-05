# SPDX-License-Identifier: Apache-2.0
"""
modbus_slave.py -- SoundDog 从站模型核心（纯 Python，无 pymodbus 依赖，可直接离线测试）。

关键事实（源: modbus_app.c L86-91 / L97-111 + debounce.h + param.h）：
  - 9 个保持寄存器（帧地址 0..8）：
      0x0000 TH          = threshold×100, 默认 600(6.0)        [读写]
      0x0001 ALARM_CNT   = consec_anom_alarm, 默认 30          [读写]
      0x0002 RELEASE_CNT = consec_norm_release, 默认 60         [读写]
      0x0003 WIN_LEN     = DB_WIN_LEN, 固定 100（拒写）          [只读]
      0x0004 WIN_ANOM    = win_anom_cnt, 默认 30                [读写]
      0x0005 TEMP_C      = temp_x10 + 500 偏移, 默认 500(=0.0℃) [只读遥测]
      0x0006 HUMI_RH     = humi_x10, 默认 0                     [只读遥测]
      0x0007 VIB_RMS     = 振动 RMS(mg), 默认 0（MPU6050 未接入） [只读遥测]
      0x0008 VIB_PEAK    = 振动峰值(mg), 默认 0                  [只读遥测]
  - 写 = 参数覆盖(A4-04)。范围校验:
      帧0 TH:      [100, 2000]
      帧1/2/4:     [1, 100]
      帧3 WIN_LEN: 仅接受 100（写其它 → 拒）
      帧5..8:      **只读**，写 → 异常 0x02（从站 reg_off_to_pid() 落 default(0xFF)
                   ⇒ MB_ENOREG，见 modbus_app.c:103-106 注释）
   整批写时任一越界 → 整批拒绝(原子)，全保留旧值（modbus_app.c:102-111 两遍校验）。
  - 输入/线圈/离散: SoundDog 不暴露 → 对应请求返回 IllegalFunction（MC app L131-150）。
  - 地址基址: 帧地址 0 基址 → pymodbus 协议地址直接用 0..8。

设计定位：
  - SoundDogModel 是"语义真相"（读/写+范围+原子），纯逻辑、零依赖 → 离线回归目标。
  - pymodbus server 只是"能把模型挂上网线"的薄壳，语义全部委托本模块，不依赖其 datastore 内部。
  三源调研：pymodbus 3.15 已拔除旧 datastore 读写语义（读 context.py/sequential.py 见 garbage）；
  与其绑易变库 API，不如把语义收进口自己可控的核心。
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import List

from al_sim import DebounceSim, DebounceParam

# ---- 参数范围（SoundDog param.h, 禁止自造） ----
TH_MIN_X100 = 100
TH_MAX_X100 = 2000
FRAME_MIN = 1
FRAME_MAX = 100
WIN_LEN_FIXED = 100
REG_COUNT = 9          # 0..4 参数 + 5..8 只读遥测（从站 MODBUS_REG_HOLDING_NUM）
TELEMETRY_BASE = 5     # 5..8 只读：写 → 异常 0x02（MB_ENOREG）


class SdException(Exception):
    """SoundDog 从站异常(异常码), 继承 Exception 以便 pytest.raises。"""

    def __init__(self, code: int, msg: str = ""):
        super().__init__(f"SoundDog 0x{code:02X} {msg}".rstrip())
        self.code = code

    @classmethod
    def ILLEGAL_FUNCTION(cls):      # noqa: N802 (风格镜像 Modbus 名词)
        return cls(0x01)

    @classmethod
    def ILLEGAL_DATA_ADDRESS(cls):  # noqa: N802
        return cls(0x02)

    @classmethod
    def ILLEGAL_DATA_VALUE(cls):    # noqa: N802
        return cls(0x03)


@dataclass
class SoundDogModel:
    """SoundDog 保持寄存器语义核心（读快照/写校验/原子/输入线圈不暴露）。

    默认参数 = debounce.h 宏; 通过 set_param 可态调（对齐 A4-04 / debounce_set_params）。
    """
    sim: DebounceSim = field(default_factory=DebounceSim)

    # ---- 只读遥测（对应从站 g_temp_x10 / g_humi_x10 / g_vib_rms / g_vib_peak）----
    # 默认 0：温度在寄存器里要 +500 偏移 ⇒ 默认 500 表示 0.0℃（与从站一致）
    temp_x10: int = 0
    humi_x10: int = 0
    vib_rms: int = 0
    vib_peak: int = 0

    def _snapshot(self) -> List[int]:
        """0..8 完整快照：前 5 个来自参数仿真，后 4 个为只读遥测。"""
        return list(self.sim.holding_regs()) + [
            self.temp_x10 + 500,   # 0x0005 TEMP_C：×10 加 500 偏移
            self.humi_x10,         # 0x0006 HUMI_RH
            self.vib_rms,          # 0x0007 VIB_RMS
            self.vib_peak,         # 0x0008 VIB_PEAK
        ]

    # ---- 读 0x03：返回 0 基址 帧[addr..addr+n)，越界 → 异常 ----
    def read_holdings(self, addr: int, n: int = 1):
        if addr < 0 or addr + n > REG_COUNT:
            raise SdException.ILLEGAL_DATA_ADDRESS()
        return self._snapshot()[addr:addr + n]

    # ---- 写 0x10：整批原子，任一越界拒写（两遍校验对齐 MC L102-111） ----
    def write_holdings(self, addr: int, values: List[int]) -> None:
        if addr < 0 or addr + len(values) > REG_COUNT:
            raise SdException.ILLEGAL_DATA_ADDRESS()
        # 只读遥测 5..8：从站 reg_off_to_pid() 落 default(0xFF) ⇒ MB_ENOREG(0x02)
        if any((addr + i) >= TELEMETRY_BASE for i in range(len(values))):
            raise SdException.ILLEGAL_DATA_ADDRESS()
        # 第一遍：全量校验，任一非法 → 拒绝（旧值全保留）
        for i, v in enumerate(values):
            if not self._reg_is_valid(addr + i, v):
                raise SdException.ILLEGAL_DATA_VALUE()
        # 第二遍：全部合法才逐一生效（还原为参数）
        for i, v in enumerate(values):
            self._apply_reg(addr + i, v)

    def _reg_is_valid(self, reg: int, val: int) -> bool:
        if reg == 0:
            return TH_MIN_X100 <= int(val) <= TH_MAX_X100
        if reg in (1, 2, 4):
            return FRAME_MIN <= int(val) <= FRAME_MAX
        if reg == 3:
            return int(val) == WIN_LEN_FIXED      # 固定100, 其它拒
        return False

    def _apply_reg(self, reg: int, val: int) -> None:
        p = self.sim.get_param()
        if reg == 0:
            p.threshold = int(val) / 100.0
        elif reg == 1:
            p.consec_anom_alarm = int(val)
        elif reg == 2:
            p.consec_norm_release = int(val)
        elif reg == 4:
            p.win_anom_cnt = int(val)
        # reg==3 拒写（_reg_is_valid 已挡）
        self.sim.set_param(p)

    # ---- 只读对象不支持(输入/线圈/离散) → 冒烟层映射为 Illegal Function ----
    @property
    def read_unsupported(self):
        return SdException.ILLEGAL_FUNCTION()


# 便捷别名：验证文档引用的命名
SoundDogSlave = SoundDogModel