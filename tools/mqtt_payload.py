# SPDX-License-Identifier: Apache-2.0
"""
mqtt_payload.py -- FEAT-ESPGW-02 MQTT 发布 payload 的纯 Python 镜像。

真实固件在 main/wifi_mqtt.c 的 gateway_mqtt_publish() 里用 snprintf 打包 9×Holding
JSON。本模块用相同格式逐字符镜像该打包逻辑，作为离线回归基线——验证字段顺序、
字段名、取值映射与原 x 保持一致，并锁定长度边界（C 端 snprintf 缓冲 256B，
超界即拒发）。

镜像一致性判据（test 断言）：
  - 字段顺序 th,alarm_cnt,release_cnt,win_len,win_anom,temp,humi,vib_rms,vib_peak,ok
    与 C snprintf 完全一致
  - 字段名 = mb_param.h 结构体成员名；ok 为本轮成功读取数（非寄存器，元数据）
  - 0..4 原样落出（%u）；5..8 为只读遥测，temp/humi 在打包时做偏移/缩放换算
    （reg5=温度×10+500 ⇒ 753→25.3；reg6=湿度×10 ⇒ 612→61.2）
  - PAYLOAD_FMT 由 test_payload_fmt_matches_c_source 从 main/wifi_mqtt.c 反查，
    改 C 不改这里（或反之）都会红，杜绝"测试全绿但镜像已漂移"

不接串口、不依赖固件；`python -m pytest tools/test_mqtt_payload.py -v` 即跑。
"""

# 与 main/wifi_mqtt.c snprintf 完全一致的打包格式（改动会被反查测试拦截）
PAYLOAD_FMT = ('{"th":%u,"alarm_cnt":%u,"release_cnt":%u,"win_len":%u,"win_anom":%u,'
               '"temp":%.1f,"humi":%.1f,"vib_rms":%u,"vib_peak":%u,"ok":%u}')

# 与 C 端 gateway_mqtt_publish 缓冲一致；超界 C 端会拒发，这里同判据
C_PAYLOAD_BUF = 256

# 一轮成功读取全部 9 个寄存器（MASTER_PARAM_CNT）；ok<9 表示混合快照，C 端会 WARN
OK_ALL_PARAMS = 9

# 与 mb_param.h 的 SD_TEMP_OFFSET / SD_TEMP_SCALE / SD_HUMI_SCALE 保持一致
SD_TEMP_OFFSET = 500
SD_TEMP_SCALE = 10.0
SD_HUMI_SCALE = 10.0


class HoldingSnapshot:
    """镜像 mb_param.h 的 sounddog_holding_t（9 × u16 字段，顺序不可改）。

    0..4 为可写参数，5..8 为只读遥测（原始寄存器值，未做换算）。
    """

    __slots__ = ("th", "alarm_cnt", "release_cnt", "win_len", "win_anom",
                 "temp", "humi", "vib_rms", "vib_peak")

    def __init__(self, th=0, alarm_cnt=0, release_cnt=0, win_len=0, win_anom=0,
                 temp=0, humi=0, vib_rms=0, vib_peak=0):
        self.th = th
        self.alarm_cnt = alarm_cnt
        self.release_cnt = release_cnt
        self.win_len = win_len
        self.win_anom = win_anom
        self.temp = temp
        self.humi = humi
        self.vib_rms = vib_rms
        self.vib_peak = vib_peak

    def field_order(self):
        """字段顺序 = 打包顺序（与 C snprintf 逐字段对应；勿改）。"""
        return ("th", "alarm_cnt", "release_cnt", "win_len", "win_anom",
                "temp", "humi", "vib_rms", "vib_peak")


def build_payload(regs, ok_count=OK_ALL_PARAMS):
    """镜像 C 端 snprintf 打包逻辑，返回与固件一致的 JSON 字符串。

    ok_count 是元数据（本轮成功读取份数），不属于 sounddog_holding_t，
    故不入 HoldingSnapshot，单独入参——保持"快照镜像结构体"的不变量。
    temp/humi 的偏移与缩放在此完成，与 C 端表达式逐项对应。
    """
    return PAYLOAD_FMT % (
        regs.th, regs.alarm_cnt, regs.release_cnt,
        regs.win_len, regs.win_anom,
        (regs.temp - SD_TEMP_OFFSET) / SD_TEMP_SCALE,
        regs.humi / SD_HUMI_SCALE,
        regs.vib_rms, regs.vib_peak, ok_count,
    )


def payload_fits_buffer(payload):
    """C 端用 256B 静态缓冲，n<0 或 n>=256 拒发，这里同判据。"""
    return 0 <= len(payload) < C_PAYLOAD_BUF


# ======================================================================
# 旧版：自建 / 公共 broker 时代的「下行控制回执」payload（**已停用，仅存档**）
# ----------------------------------------------------------------------
# ⚠️ 2026-10-27 接入 ThingsCloud 后，ack 不再发布：ThingsCloud 没有"命令 ACK 主题"，
#    且属性上报只接受功能定义里已声明的键，擅自发 {"cid":..,"err":..} 会被 406 拒收
#    并可能断连。C 端 gateway_mqtt_publish_ack() 已改为【只打本地日志】，
#    snprintf(ack,...) 整段删除。
#    本节的格式串保留，仅用于记录"若改回自建 broker 该发什么"，
#    **不再与 C 源码做反查对照**（对应护栏已改为 test_ack_not_published）。
# ======================================================================

# 旧格式（自建 broker）：与当年 C 端 snprintf(ack, sizeof(ack), ...) 一致
ACK_FMT = '{"cid":%u,"value":%u,"err":%d}'

# 旧缓冲大小（自建 broker 时代 C 端 char ack[96]）
C_ACK_BUF = 96


def build_ack(cid, value, err):
    """镜像 C 端 ack 打包：cid=寄存器号，value=下发值，err=esp_err_t 原值。"""
    return ACK_FMT % (cid, value, err)


def ack_fits_buffer(ack):
    """C 端 96B 静态缓冲，n<0 或 n>=96 拒发，这里同判据。"""
    return 0 <= len(ack) < C_ACK_BUF