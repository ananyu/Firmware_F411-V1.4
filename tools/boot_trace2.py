#!/usr/bin/env python3
"""boot_trace2.py - ordered boot-path trace, HARDWARE breakpoints only.

F411 has 6 hw bp. Software bp would collide with the blackbox's own
flash writes. On every halt: name, time, xTickCount (reset detection),
RCC_CSR (reset cause). Run AFTER flashing the image matching the map.
"""
import os
import re
import sys
import time

from pyocd.core.helpers import ConnectHelper

HERE = os.path.dirname(os.path.abspath(__file__))
MAP = os.path.join(os.path.dirname(HERE), "OBJ", "Firmware_F411.map")

WANT = ["systemInit", "blackboxInit", "bbxEraseSector", "printHardFault",
        "vTaskStartScheduler", "startTask"]
TICK = 0x200004FC
RCC_CSR = 0x40023874

def load_syms():
    syms = {}
    pat = re.compile(r'^\s+(\S+)\s+(0x[0-9a-f]{8})\s+Thumb Code\s+\d+\s+\S+')
    with open(MAP, "rb") as f:
        text = f.read().decode("gbk", errors="replace")
    for line in text.splitlines():
        m = pat.match(line)
        if m and m.group(1) in WANT:
            syms[m.group(1)] = int(m.group(2), 16) & ~1
    return syms

def main():
    syms = load_syms()
    sess = ConnectHelper.session_with_chosen_probe(
        target_override="stm32f411ce", connect_mode="attach")
    with sess:
        t = sess.target
        print("hw breakpoints available:", t.number_of_breakpoints)
        t.reset()
        t.halt()
        addr2name = {a: n for n, a in syms.items()}
        for name, a in syms.items():
            t.set_breakpoint(a, type="hw")
        t.resume()
        t0 = time.time()
        once = set()
        while time.time() - t0 < 12.0:
            time.sleep(0.01)
            if not (t.read32(0xE000EDF0) & (1 << 1)):
                continue
            pc = t.read_core_registers_raw([15])[0] & ~1
            name = addr2name.get(pc, f"0x{pc:08x}")
            tick = t.read32(TICK)
            csr = t.read32(RCC_CSR)
            print(f"{time.time()-t0:6.2f}s  {name:20s} tick={tick} "
                  f"csr=0x{csr:08x}")
            if name not in once:
                once.add(name)
                try:
                    t.remove_breakpoint(pc)
                except Exception:
                    pass
            t.resume()
        t.halt()
        for a in addr2name:
            try:
                t.remove_breakpoint(a)
            except Exception:
                pass
        t.resume()
        print("\nseen:", sorted(once))
    return 0

if __name__ == "__main__":
    sys.exit(main())
