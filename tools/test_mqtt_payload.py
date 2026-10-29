# SPDX-License-Identifier: Apache-2.0
"""
test_mqtt_payload.py -- FEAT-ESPGW-02 MQTT payload 结构回归测试。

验证 gateway_mqtt_publish() 的 JSON 打包在离线侧与原 x 一致：字段顺序/字段名/
取值映射、前后缀精确匹配、长度缓冲边界。作为修改 main/wifi_mqtt.c 打包逻辑时的
回归护栏（防止字段名/顺序被改坏而固件与订阅端对不上）。

`python -m pytest tools/test_mqtt_payload.py -v` 即跑。
"""
import pathlib
import re

import pytest
import json

from mqtt_payload import (HoldingSnapshot, build_payload, payload_fits_buffer,
                          PAYLOAD_FMT, OK_ALL_PARAMS,
                          build_ack, ack_fits_buffer, ACK_FMT, C_ACK_BUF)

C_SRC = pathlib.Path(__file__).resolve().parent.parent / "main" / "wifi_mqtt.c"


def _extract_literals_after(src, pattern, what):
    """取出 pattern 之后的连续字符串字面量并去转义（C 侧多行拼接）。"""
    m = re.search(pattern, src)
    assert m, what
    pos, parts = m.end(), []
    lit = re.compile(r'\s*"((?:[^"\\]|\\.)*)"')
    while True:
        lm = lit.match(src, pos)
        if not lm:
            break
        parts.append(lm.group(1))
        pos = lm.end()
    assert parts, what + "（snprintf 后未找到任何字符串字面量）"
    return "".join(parts).replace('\\"', '"')


def _extract_c_ack_fmt():
    """FEAT-ESPGW-05：从 gateway_mqtt_publish_ack 的 snprintf(ack,...) 反查格式串。

    ⚠️ 必须锚在 `ack`，不能锚 `payload` —— 发布侧那条是另一个护栏的地盘。
    """
    return _extract_literals_after(
        C_SRC.read_text(encoding="utf-8"),
        r"snprintf\s*\(\s*ack\s*,\s*sizeof\(ack\)\s*,\s*",
        "在 main/wifi_mqtt.c 里找不到 snprintf(ack, sizeof(ack), ...)")


def _extract_c_payload_fmt():
    """从 main/wifi_mqtt.c 的 snprintf 里反查实际打包格式串。

    C 侧格式串是多行相邻字面量拼接，这里把连续字符串字面量取出并去转义，
    得到固件真正发出的格式，用于与 PAYLOAD_FMT 对撞（防镜像漂移）。
    """
    src = C_SRC.read_text(encoding="utf-8")
    m = re.search(r"snprintf\s*\(\s*payload\s*,\s*sizeof\(payload\)\s*,\s*", src)
    assert m, "在 main/wifi_mqtt.c 里找不到 snprintf(payload, sizeof(payload), ...)"
    pos, parts = m.end(), []
    lit = re.compile(r'\s*"((?:[^"\\]|\\.)*)"')
    while True:
        lm = lit.match(src, pos)
        if not lm:
            break
        parts.append(lm.group(1))
        pos = lm.end()
    assert parts, "snprintf 后未找到任何字符串字面量"
    return "".join(parts).replace('\\"', '"')


@pytest.fixture()
def snapshot():
    return HoldingSnapshot(th=600, alarm_cnt=30, release_cnt=60,
                           win_len=100, win_anom=30,
                           temp=753, humi=612, vib_rms=0, vib_peak=0)


def test_payload_fmt_matches_c_source():
    """反漂移：PAYLOAD_FMT 必须逐字符等于 C 源码里的实际格式串。

    这是本文件的护栏之护栏——只断言硬编码期望串的话，改了 C 不改这里依然全绿，
    镜像悄悄失效却无人知晓（正是 R25 警示的"绿≠对"）。
    """
    assert PAYLOAD_FMT == _extract_c_payload_fmt()


def test_payload_matches_c_format(snapshot):
    """打包结果逐字符等于 C snprintf 期望串（含 ok 元数据）。"""
    assert build_payload(snapshot) == (
        '{"th":600,"alarm_cnt":30,"release_cnt":60,"win_len":100,"win_anom":30,'
        '"temp":25.3,"humi":61.2,"vib_rms":0,"vib_peak":0,"ok":9}')


def test_payload_is_valid_json_and_field_order(snapshot):
    """是合法 JSON，且顶层字段顺序与 mb_param.h 完全一致（ok 末尾）。"""
    s = build_payload(snapshot)
    obj = json.loads(s)
    assert list(obj.keys()) == ["th", "alarm_cnt", "release_cnt", "win_len",
                                "win_anom", "temp", "humi", "vib_rms",
                                "vib_peak", "ok"]
    assert obj["th"] == 600
    assert obj["win_anom"] == 30
    assert obj["ok"] == OK_ALL_PARAMS


def test_temp_humi_offset_conversion():
    """遥测换算：从站 reg5 = 温度×10+500，reg6 = 湿度×10。

    这是本镜像最容易漂移的一处——偏移量写错会让云端稳定显示错值，
    而且"看起来像正常温度"，比报错更难发现。
    """
    # 25.3℃ ⇒ 753；0.0℃ ⇒ 500；-45.0℃ ⇒ 50（int16 负温经 +500 抬到正整数区）
    assert json.loads(build_payload(HoldingSnapshot(temp=753, humi=612)))["temp"] == 25.3
    assert json.loads(build_payload(HoldingSnapshot(temp=753, humi=612)))["humi"] == 61.2
    assert json.loads(build_payload(HoldingSnapshot(temp=500)))["temp"] == 0.0
    assert json.loads(build_payload(HoldingSnapshot(temp=50)))["temp"] == -45.0


def test_field_names_match_struct_members(snapshot):
    """字段名 = sounddog_holding_t 成员名（不应被改名/拼写漂移）。"""
    assert snapshot.field_order() == ("th", "alarm_cnt", "release_cnt", "win_len",
                                      "win_anom", "temp", "humi", "vib_rms", "vib_peak")


def test_wide_range_values_preserved():
    """u16 全量程取值原样落出，不截断、不注入（遥测项按换算后落出）。"""
    big = HoldingSnapshot(th=65535, alarm_cnt=0, release_cnt=1, win_len=100, win_anom=2000,
                          temp=65535, humi=65535, vib_rms=65535, vib_peak=65535)
    obj = json.loads(build_payload(big))
    assert obj == {"th": 65535, "alarm_cnt": 0, "release_cnt": 1,
                   "win_len": 100, "win_anom": 2000,
                   "temp": 6503.5, "humi": 6553.5,
                   "vib_rms": 65535, "vib_peak": 65535, "ok": 9}


def test_partial_round_reports_ok():
    """部分成功时 ok<9 如实落出——上位机据此识别混合快照（P-4 残留-1）。"""
    s = HoldingSnapshot(th=600, alarm_cnt=30, release_cnt=60, win_len=100, win_anom=30)
    assert json.loads(build_payload(s, 2))["ok"] == 2
    assert json.loads(build_payload(s, 1))["ok"] == 1


def test_payload_fits_256B_buffer(snapshot):
    """长度不越 256B 缓冲临界（C 端 snprintf 拒发判据）。"""
    s = build_payload(snapshot)
    assert payload_fits_buffer(s) is True
    assert len(s) < 256

    # 最大 u16 + ok 满载情况下边界仍安全
    worst = build_payload(HoldingSnapshot(th=65535, alarm_cnt=65535, release_cnt=65535,
                                          win_len=65535, win_anom=65535,
                                          temp=65535, humi=65535,
                                          vib_rms=65535, vib_peak=65535),
                          OK_ALL_PARAMS)
    assert payload_fits_buffer(worst) is True
    assert len(worst) < 256


# ======================================================================
# FEAT-ESPGW-05：下行控制回执 payload
# ======================================================================

def test_ack_not_published():
    """ThingsCloud 无 ack 主题：ack 只走本地日志，C 端不得再出现 snprintf(ack,...)。

    2026-10-27 接入 ThingsCloud 时删除了 ack 发布——发未定义的键会被平台 406 拒收
    并可能断连。本条护栏防"顺手把 ack 发布加回来"，那会把已跑通的云端链路搞坏。
    ⚠️ 2026-10-29 复核发现：上一版这里是 `ACK_FMT == _extract_c_ack_fmt()` 的反查断言，
    在删除 ack 发布后一直红着（提交 97f646c 未跑测试），已改为"断言其不存在"。
    """
    src = C_SRC.read_text(encoding="utf-8")
    assert "snprintf(ack" not in src, "ack 发布被重新加入，ThingsCloud 会 406 拒收"
    assert "gateway_mqtt_publish_ack" in src, "ack 本地日志函数不应被删掉"


def test_ack_matches_c_format():
    """逐字符等于 C snprintf 期望串。"""
    assert build_ack(0, 650, 0) == '{"cid":0,"value":650,"err":0}'


def test_ack_encodes_error_code():
    """err 为 esp_err_t 原值：0=成功、0x102 参数非法、0x103 不支持、0x107 超时。"""
    assert json.loads(build_ack(3, 100, 0x103)) == {"cid": 3, "value": 100, "err": 259}
    assert json.loads(build_ack(9, 0, 0x102)) == {"cid": 9, "value": 0, "err": 258}
    assert json.loads(build_ack(0, 65535, 0x107))["err"] == 0x107


def test_ack_is_valid_json_with_fixed_field_order():
    """顶层字段顺序 cid,value,err —— 与 C snprintf 一致（订阅端可能按序解析）。"""
    s = build_ack(4, 30, 0)
    assert list(json.loads(s).keys()) == ["cid", "value", "err"]


def test_ack_fits_96B_buffer():
    """长度不越 96B 缓冲临界；最坏情况（全 u16 最大 + 4 位错误码）仍安全。"""
    assert ack_fits_buffer(build_ack(0, 0, 0)) is True
    worst = build_ack(65535, 65535, -2147483648)
    assert ack_fits_buffer(worst) is True
    assert len(worst) < C_ACK_BUF