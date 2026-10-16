#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# 一次性调试脚本（中危-13 标注）：从 PC 发 Modbus 帧、抓 SoundDog UART1 的 RX[] 行。
# 硬编码 COM 口（host=COM5 适配器, dbg=COM4 SoundDog 日志），非正式工具。
import serial, time, re

HOST = "COM5"    # USB-RS485 适配器（PC 主站）
DBG  = "COM4"    # SoundDog UART1 调试日志

def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if (crc & 1) else (crc >> 1)
    return crc & 0xFFFF

# 读保持寄存器 0x0000，数量 1：地址01 功能03 起始00 00 数量00 01
req = bytes([0x01, 0x03, 0x00, 0x00, 0x00, 0x01])
crc = crc16(req)
frame = req + bytes([crc & 0xFF, crc >> 8])
print("发送帧:", frame.hex(" ").upper())

host = serial.Serial(HOST, 115200, timeout=0.2)
dbg  = serial.Serial(DBG,  115200, timeout=0.2)

# --- 发送期间强制使能适配器 RS485 方向（用 pyserial setRTS/setDTR，勿用 setattr 大写属性）---
def de_high():
    for name, setter in (("RTS", host.setRTS), ("DTR", host.setDTR)):
        try: setter(True)
        except Exception as e: print("  DE set fail:", name, e)
def de_low():
    for name, setter in (("RTS", host.setRTS), ("DTR", host.setDTR)):
        try: setter(False)
        except Exception as e: print("  DE set fail:", name, e)

de_high()
time.sleep(0.3); dbg.reset_input_buffer(); host.reset_input_buffer()

t0 = time.time()
next_send = 0
rx_lines = []
while time.time() - t0 < 4.0:
    now = time.time()
    if now >= next_send:
        de_high()
        host.write(frame)          # 发一帧
        host.flush()
        time.sleep(0.01)
        de_low()                   # 发完回接收态
        next_send = now + 0.4      # 每 400ms 发一次
    chunk = dbg.read(512)
    if chunk:
        for line in chunk.decode("latin-1", "ignore").splitlines():
            if "RX[" in line:
                rx_lines.append(line)
                print("[RX]", line)
host.close(); dbg.close()
print("--- 结束(带RTS) ---")
print("RX 帧行统计:", len(rx_lines), "条")
for l in rx_lines:
    print(l)