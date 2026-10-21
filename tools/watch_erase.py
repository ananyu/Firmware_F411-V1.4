#!/usr/bin/env python3
"""watch_erase.py - reset with S4 blank, then PURE-READ poll of the
self-format (erase) branch. No halts/bps. S4 flash read is fault-tolerant
(reading flash during an erase stalls the debug AP)."""
import subprocess
import sys
import time

from pyocd.core.helpers import ConnectHelper

A = {"tick": 0x200004FC, "tasks": 0x200004F8, "booted": 0x200007E9,
     "stage": 0x200007EA, "magicRead": 0x200007EC, "eraseSr": 0x200007F0,
     "eraseW0": 0x200007F4, "progW0": 0x200007FC, "scanEnd": 0x20000800,
     "word": 0x200007DC, "seq": 0x200007E0, "rcc": 0x40023874}
S4 = 0x08010000

def safe32(t, a):
    try:
        return t.read32(a)
    except Exception:
        return None

def main():
    secs = int(sys.argv[1]) if len(sys.argv) > 1 else 12
    subprocess.run(["pyocd", "reset", "-t", "stm32f411ce"], capture_output=True)
    sess = ConnectHelper.session_with_chosen_probe(
        target_override="stm32f411ce", connect_mode="attach")
    with sess:
        t = sess.target
        t0 = time.time()
        while time.time() - t0 < secs:
            dt = time.time() - t0
            tick = t.read32(A["tick"])
            stage = t.read8(A["stage"])
            booted = t.read8(A["booted"])
            rcc = t.read32(A["rcc"])
            s4 = safe32(t, S4)
            esr = t.read32(A["eraseSr"])
            ew0 = t.read32(A["eraseW0"])
            pw0 = t.read32(A["progW0"])
            tasks = t.read32(A["tasks"])
            s4s = ("0x%08x" % s4) if s4 is not None else "FAULT"
            print("%5.1fs tick=%-6d tasks=%-2d stage=%d booted=%d "
                  "S4=%s eraseSr=0x%x eraseW0=0x%08x progW0=0x%08x rcc=0x%08x"
                  % (dt, tick, tasks, stage, booted, s4s, esr, ew0, pw0, rcc))
            time.sleep(0.5)
    return 0

if __name__ == "__main__":
    sys.exit(main())
