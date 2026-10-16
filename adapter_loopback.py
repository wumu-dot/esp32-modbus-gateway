#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# 一次性调试脚本（中危-13 标注）：自回环验证 USB-RS485 适配器能否真驱动 A/B。
# 硬编码 COM5/COM 口，非正式工具；使用 python3 adapter_loopback.py
import serial, time

def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if (crc & 1) else (crc >> 1)
    return crc & 0xFFFF

req = bytes([0x01, 0x03, 0x00, 0x00, 0x00, 0x01])
crc = crc16(req)
frame = req + bytes([crc & 0xFF, crc >> 8])

s = serial.Serial("COM5", 115200, timeout=2)
s.reset_input_buffer()

def de_high():
    for name, setter in (("RTS", s.setRTS), ("DTR", s.setDTR)):
        try: setter(True); print(f"  [cfg] {name}=True")
        except Exception as e: print(f"  [cfg] {name} set fail: {e}")
def de_low():
    for name, setter in (("RTS", s.setRTS), ("DTR", s.setDTR)):
        try: setter(False)
        except Exception as e: print(f"  [cfg] {name} set fail: {e}")

for i in range(5):
    de_high()
    s.write(frame); s.flush()
    time.sleep(0.1)
    de_low()
    echo = s.read(32)
    print(f"[send#{i}] 发出={frame.hex(' ').upper()}  读回={echo.hex(' ').upper() if echo else '(无/NONE)'}")
s.close()