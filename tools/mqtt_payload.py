# SPDX-License-Identifier: Apache-2.0
"""
mqtt_payload.py -- FEAT-ESPGW-02 MQTT 发布 payload 的纯 Python 镜像。

真实固件在 main/wifi_mqtt.c 的 gateway_mqtt_publish() 里用 snprintf 打包 5×Holding
JSON。本模块用相同格式逐字符镜像该打包逻辑，作为离线回归基线——验证字段顺序、
字段名、取值映射与原 x 保持一致，并锁定长度边界（C 端 snprintf 缓冲 192B，
超界即拒发）。

镜像一致性判据（test 断言）：
  - 字段顺序 th,alarm_cnt,release_cnt,win_len,win_anom,ok 与 C snprintf 完全一致
  - 字段名 = mb_param.h 结构体成员名；ok 为本轮成功读取数（非寄存器，元数据）
  - 任一字段取值原样落出（%u 无符号十进制，不做 +/- 注入）
  - PAYLOAD_FMT 由 test_payload_fmt_matches_c_source 从 main/wifi_mqtt.c 反查，
    改 C 不改这里（或反之）都会红，杜绝"测试全绿但镜像已漂移"

不接串口、不依赖固件；`python -m pytest tools/test_mqtt_payload.py -v` 即跑。
"""

# 与 main/wifi_mqtt.c snprintf 完全一致的打包格式（改动会被反查测试拦截）
PAYLOAD_FMT = '{"th":%u,"alarm_cnt":%u,"release_cnt":%u,"win_len":%u,"win_anom":%u,"ok":%u}'

# 与 C 端 gateway_mqtt_publish 缓冲一致；超界 C 端会拒发，这里同判据
C_PAYLOAD_BUF = 192

# 一轮成功读取全部 5 个参数（MASTER_PARAM_CNT）；ok<5 表示混合快照，C 端会 WARN
OK_ALL_PARAMS = 5


class HoldingSnapshot:
    """镜像 mb_param.h 的 sounddog_holding_t（5 × u16 字段，顺序不可改）。"""

    __slots__ = ("th", "alarm_cnt", "release_cnt", "win_len", "win_anom")

    def __init__(self, th=0, alarm_cnt=0, release_cnt=0, win_len=0, win_anom=0):
        self.th = th
        self.alarm_cnt = alarm_cnt
        self.release_cnt = release_cnt
        self.win_len = win_len
        self.win_anom = win_anom

    def field_order(self):
        """字段顺序 = 打包顺序（与 C snprintf 逐字段对应；勿改）。"""
        return ("th", "alarm_cnt", "release_cnt", "win_len", "win_anom")


def build_payload(regs, ok_count=OK_ALL_PARAMS):
    """镜像 C 端 snprintf 打包逻辑，返回与固件一致的 JSON 字符串。

    ok_count 是元数据（本轮成功读取份数），不属于 sounddog_holding_t，
    故不入 HoldingSnapshot，单独入参——保持"快照镜像结构体"的不变量。
    """
    return PAYLOAD_FMT % (
        regs.th, regs.alarm_cnt, regs.release_cnt,
        regs.win_len, regs.win_anom, ok_count,
    )


def payload_fits_buffer(payload):
    """C 端用 192B 静态缓冲，n<0 或 n>=192 拒发，这里同判据。"""
    return 0 <= len(payload) < C_PAYLOAD_BUF