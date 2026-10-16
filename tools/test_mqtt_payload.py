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
                          PAYLOAD_FMT, OK_ALL_PARAMS)

C_SRC = pathlib.Path(__file__).resolve().parent.parent / "main" / "wifi_mqtt.c"


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
                           win_len=100, win_anom=30)


def test_payload_fmt_matches_c_source():
    """反漂移：PAYLOAD_FMT 必须逐字符等于 C 源码里的实际格式串。

    这是本文件的护栏之护栏——只断言硬编码期望串的话，改了 C 不改这里依然全绿，
    镜像悄悄失效却无人知晓（正是 R25 警示的"绿≠对"）。
    """
    assert PAYLOAD_FMT == _extract_c_payload_fmt()


def test_payload_matches_c_format(snapshot):
    """打包结果逐字符等于 C snprintf 期望串（含 ok 元数据）。"""
    assert build_payload(snapshot) == (
        '{"th":600,"alarm_cnt":30,"release_cnt":60,"win_len":100,"win_anom":30,"ok":5}')


def test_payload_is_valid_json_and_field_order(snapshot):
    """是合法 JSON，且顶层字段顺序与 mb_param.h 完全一致（ok 末尾）。"""
    s = build_payload(snapshot)
    obj = json.loads(s)
    assert list(obj.keys()) == ["th", "alarm_cnt", "release_cnt", "win_len",
                                "win_anom", "ok"]
    assert obj["th"] == 600
    assert obj["win_anom"] == 30
    assert obj["ok"] == OK_ALL_PARAMS


def test_field_names_match_struct_members(snapshot):
    """字段名 = sounddog_holding_t 成员名（不应被改名/拼写漂移）。"""
    assert snapshot.field_order() == ("th", "alarm_cnt", "release_cnt", "win_len", "win_anom")


def test_wide_range_values_preserved():
    """u16 全量程取值原样落出，不截断、不注入。"""
    big = HoldingSnapshot(th=65535, alarm_cnt=0, release_cnt=1, win_len=100, win_anom=2000)
    obj = json.loads(build_payload(big))
    assert obj == {"th": 65535, "alarm_cnt": 0, "release_cnt": 1,
                   "win_len": 100, "win_anom": 2000, "ok": 5}


def test_partial_round_reports_ok():
    """部分成功时 ok<5 如实落出——上位机据此识别混合快照（P-4 残留-1）。"""
    s = HoldingSnapshot(th=600, alarm_cnt=30, release_cnt=60, win_len=100, win_anom=30)
    assert json.loads(build_payload(s, 2))["ok"] == 2
    assert json.loads(build_payload(s, 1))["ok"] == 1


def test_payload_fits_192B_buffer(snapshot):
    """长度不越 192B 缓冲临界（C 端 snprintf 拒发判据）。"""
    s = build_payload(snapshot)
    assert payload_fits_buffer(s) is True
    assert len(s) < 192

    # 最大 u16 + ok 满载情况下边界仍安全
    worst = build_payload(HoldingSnapshot(th=65535, alarm_cnt=65535, release_cnt=65535,
                                          win_len=65535, win_anom=65535),
                          OK_ALL_PARAMS)
    assert payload_fits_buffer(worst) is True
    assert len(worst) < 192