import serial, time

ports = ["COM3", "COM4", "COM5"]
for p in ports:
    print(f"===== {p} =====")
    try:
        s = serial.Serial(p, 115200, timeout=0.2)
        time.sleep(0.3)
        s.reset_input_buffer()
    except Exception as e:
        print("  open failed:", e)
        continue
    buf = b""
    t0 = time.time()
    while time.time() - t0 < 2.0:
        chunk = s.read(256)
        if chunk:
            buf += chunk
        else:
            time.sleep(0.05)
    s.close()
    printable = "".join(ch if (32 <= ord(ch) < 127 or ch in "\r\n") else "." for ch in buf.decode("latin-1"))
    print("  bytes:", len(buf))
    print("  text:", printable[-400:])