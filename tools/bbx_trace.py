#!/usr/bin/env python3
"""bbx_trace.py - repeated-hit trace of the blackbox boot path.

Unlike boot_trace2 (one-shot), breakpoints here STAY armed so we can see
re-entry / reset loops. On each halt: time, name, tick, RCC_CSR, LR, R0/R1.
Addresses are parsed live from OBJ\Firmware_F411.map (& ~1).
"""
import os
import re
import sys
import time

from pyocd.core.helpers import ConnectHelper

HERE = os.path.dirname(os.path.abspath(__file__))
MAP = os.path.join(os.path.dirname(HERE), "OBJ", "Firmware_F411.map")

WANT = ["startTask", "blackboxInit", "bbxEraseSector", "bbxProgramWord",
        "bbxRecoverCursor", "printHardFault"]
TICK = 0x200004FC
RCC_CSR = 0x40023874
BBX_BOOTED = 0x200007E9

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
    print("syms:", {k: hex(v) for k, v in syms.items()})
    sess = ConnectHelper.session_with_chosen_probe(
        target_override="stm32f411ce", connect_mode="attach")
    with sess:
        t = sess.target
        t.reset()
        t.halt()
        addr2name = {a: n for n, a in syms.items()}
        for a in addr2name:
            t.set_breakpoint(a)
        t.resume()
        t0 = time.time()
        hits = {}
        last = 0.0
        while time.time() - t0 < 15.0:
            time.sleep(0.005)
            if not (t.read32(0xE000EDF0) & (1 << 1)):
                continue
            now = time.time() - t0
            pc = t.read_core_registers_raw([15])[0] & ~1
            lr = t.read_core_registers_raw([14])[0]
            r0, r1 = t.read_core_registers_raw([0, 1])
            tick = t.read32(TICK)
            csr = t.read32(RCC_CSR)
            booted = t.read8(BBX_BOOTED)
            name = addr2name.get(pc, "0x%08x" % pc)
            hits[name] = hits.get(name, 0) + 1
            gap = now - last
            last = now
            print("%6.2fs(gap%5.2f) %-16s hit#%d tick=%-6d csr=0x%08x "
                  "booted=%d LR=0x%08x R0=0x%08x R1=0x%08x"
                  % (now, gap, name, hits[name], tick, csr, booted, lr, r0, r1))
            t.resume()
        t.halt()
        for a in addr2name:
            try:
                t.remove_breakpoint(a)
            except Exception:
                pass
        t.resume()
        print("\nhits:", hits)
    return 0

if __name__ == "__main__":
    sys.exit(main())
