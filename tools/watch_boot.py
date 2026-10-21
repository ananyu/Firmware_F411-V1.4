#!/usr/bin/env python3
"""watch_boot.py - reset, then PURE-READ poll of boot state (no halts/bps).

Addresses from v1.0.22 map. bbxBooted=1 + S4magic=BBX1 means the
blackbox boot path completed. tick>0 means scheduler running.
"""
import subprocess
import sys
import time

from pyocd.core.helpers import ConnectHelper

A = {"tick": 0x200004FC, "tasks": 0x200004F8, "bbxBooted": 0x200007E9,
     "bbxWord": 0x200007DC, "bbxSeq": 0x200007E0,
     "bbxDrop": 0x200007E4, "bbxFull": 0x200007E8,
     "isMPU": 0x200008A4, "isMag": 0x200008A5, "isBaro": 0x200008A6,
     "fsLevel": 0x200007B4}
S4 = 0x08010000

def main():
    secs = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    subprocess.run(["pyocd", "reset", "-t", "stm32f411ce"],
                   capture_output=True)
    sess = ConnectHelper.session_with_chosen_probe(
        target_override="stm32f411ce", connect_mode="attach")
    with sess:
        t = sess.target
        t0 = time.time()
        while time.time() - t0 < secs:
            dt = time.time() - t0
            tick = t.read32(A["tick"])
            tasks = t.read32(A["tasks"])
            booted = t.read8(A["bbxBooted"])
            magic = t.read32(S4)
            mpu = t.read8(A["isMPU"])
            fs = t.read8(A["fsLevel"])
            word = t.read32(A["bbxWord"])
            print(f"{dt:5.1f}s tick={tick:7d} tasks={tasks:2d} "
                  f"bbxBooted={booted} S4magic=0x{magic:08x} "
                  f"bbxWord={word} MPU={mpu} fsLevel={fs}")
            time.sleep(0.5)
    return 0

if __name__ == "__main__":
    sys.exit(main())
