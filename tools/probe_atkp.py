#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""probe_atkp.py - find which COM port is the MiniFly remoter (ATKP 115200).
Sends a DOWN_STATUS request and listens for any AA AA uplink frame."""
import sys, time
import serial

ports = sys.argv[1:] or ["COM3","COM4","COM5","COM6","COM10","COM11"]
for p in ports:
    try:
        s = serial.Serial(p, 115200, timeout=0.3)
    except Exception as e:
        print(f"{p}: open fail ({e.__class__.__name__})")
        continue
    s.reset_input_buffer()
    # remoter firmware forwards any AA AF frame to the aircraft; also just
    # listening for its periodic UP frames is enough to identify it.
    t0 = time.time()
    buf = b""
    while time.time() - t0 < 1.5:
        buf += s.read(4096)
    s.close()
    idx = buf.find(b"\xAA\xAA")
    if idx >= 0 and len(buf) - idx >= 4:
        msgid, dlen = buf[idx+2], buf[idx+3]
        print(f"{p}: ATKP remoter detected, first frame msgID=0x{msgid:02X} len={dlen} ({len(buf)}B in 1.5s)")
    else:
        print(f"{p}: no ATKP frame ({len(buf)}B)")
