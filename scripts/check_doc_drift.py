#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_doc_drift.py —— CLAUDE.md 的 DOC-STATE 与 sdkconfig 一致性校验

为什么需要：
    `CLAUDE.md` 顶部有一行 `<!-- DOC-STATE: CHIP=... -->`，它声明"这些值来自源码"。
    但此前**没有任何脚本校验它** ⇒ 代码改了、这行不动，会静默过期（与 em 的 R2 同类）。

本版只做能可靠自动判定的部分，其余明确标"未校验"，不假装：

    CHIP        DOC-STATE 的 `ESP32-S3(N16R8)` ↔ sdkconfig `CONFIG_IDF_TARGET="esp32s3"`（归一化后比对）
    FLASH       仅报告 sdkconfig 实际值；DOC-STATE 未记录 ⇒ WARN（不 FAIL，加字段是文档决策）
    PSRAM       同上（`CONFIG_SPIRAM` + `CONFIG_SPIRAM_MODE_OCT`）
    其余字段    RTOS / BUILD / TASKS / HSE / MIN_STACK —— 本版**不校验**，显式列出避免误解为已覆盖

用法：
    python scripts/check_doc_drift.py           # 报告，恒返回 0
    python scripts/check_doc_drift.py --fail    # CHIP 不一致时返回 1（供 pre-commit）
"""
from __future__ import annotations

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOC = os.path.join(ROOT, 'CLAUDE.md')
SDK = os.path.join(ROOT, 'sdkconfig')


def read(p):
    with open(p, encoding='utf-8', errors='replace') as f:
        return f.read()


def norm_chip(s: str) -> str:
    """把 ESP32-S3 / esp32s3 / ESP32S3(N16R8) 统一成 esp32s3。"""
    s = s.lower()
    m = re.search(r'esp32-?s3|esp32-?s2|esp32-?c3|esp32-?c6|esp32', s)
    return m.group(0).replace('-', '') if m else s.strip()


def sdk_get(text: str, key: str):
    m = re.search(rf'^{re.escape(key)}=(.*)$', text, re.M)
    if not m:
        return None
    return m.group(1).strip().strip('"')


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--fail', action='store_true')
    args = ap.parse_args()

    for p in (DOC, SDK):
        if not os.path.exists(p):
            print(f'[SKIP] 缺文件: {os.path.basename(p)}（无法校验）')
            return 0

    doc = read(DOC)
    m = re.search(r'<!--\s*DOC-STATE:(.*?)-->', doc, re.S)
    if not m:
        print('[SKIP] CLAUDE.md 无 DOC-STATE 行')
        return 0
    state = ' '.join(m.group(1).split())

    def field(k):
        mm = re.search(rf'(?:^|[,\s]){re.escape(k)}=([^,]*)', state)
        return mm.group(1).strip() if mm else None

    sdk = read(SDK)
    problems, warns = [], []

    # ---- CHIP ----
    doc_chip = field('CHIP')
    sdk_chip = sdk_get(sdk, 'CONFIG_IDF_TARGET')
    print('[CHIP]')
    print(f'   DOC-STATE : {doc_chip}')
    print(f'   sdkconfig : {sdk_chip}')
    if doc_chip and sdk_chip:
        a, b = norm_chip(doc_chip), norm_chip(sdk_chip)
        ok = (a == b)
        print(f'   归一化    : {a} vs {b}  →  {"✅ 一致" if ok else "❌ 不一致"}')
        if not ok:
            problems.append(f'CHIP 不一致: DOC-STATE={doc_chip} / sdkconfig={sdk_chip}')
    else:
        warns.append('CHIP 缺任一侧，跳过')

    # ---- FLASH / PSRAM（只报告，不 FAIL）----
    print('[FLASH / PSRAM]（DOC-STATE 未记录 ⇒ 仅提示）')
    flash = sdk_get(sdk, 'CONFIG_ESPTOOLPY_FLASHSIZE')
    spiram = sdk_get(sdk, 'CONFIG_SPIRAM')
    mode = sdk_get(sdk, 'CONFIG_SPIRAM_MODE_OCT')
    print(f'   sdkconfig : FLASH={flash}  SPIRAM={spiram}  OCT={mode}')
    print(f'   DOC-STATE : FLASH={field("FLASH")}  PSRAM={field("PSRAM")}')
    if field('FLASH') is None or field('PSRAM') is None:
        warns.append(f'DOC-STATE 未记录 FLASH/PSRAM（实际 FLASH={flash}, PSRAM={spiram}/OCT={mode}）—— 建议补充')

    # ---- 其余字段：显式声明未校验 ----
    print('[其他字段] 本版不校验（避免误以为已覆盖）')
    for k in ('RTOS', 'BUILD', 'TASKS', 'HSE', 'MIN_STACK'):
        print(f'   {k:10s} = {field(k)}')

    print()
    for w in warns:
        print(f'  ⚠️  {w}')
    for p in problems:
        print(f'  ❌  {p}')
    if problems:
        print(f'===== {len(problems)} 项不一致 =====')
        return 1 if args.fail else 0
    print('===== ✅ CHIP 一致（其余字段未纳入自动校验）=====')
    return 0


if __name__ == '__main__':
    sys.exit(main())
