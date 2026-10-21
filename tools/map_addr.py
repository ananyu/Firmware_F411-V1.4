#!/usr/bin/env python3
"""map_addr.py - symbolize addresses via Keil .map 'Thumb Code' table (GBK-safe)."""
import re
import sys
import os

MAP = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "OBJ", "Firmware_F411.map")

def load_syms():
    syms = []
    pat = re.compile(r'^\s+(\S+)\s+(0x[0-9a-f]{8})\s+Thumb Code\s+(\d+)\s+(\S+)')
    with open(MAP, "rb") as f:
        text = f.read().decode("gbk", errors="replace")
    for line in text.splitlines():
        m = pat.match(line)
        if m:
            syms.append((m.group(1), int(m.group(2), 16), int(m.group(3)), m.group(4)))
    return syms

def main():
    syms = load_syms()
    for a in sys.argv[1:]:
        n = int(a, 16)
        hit = [s for s in syms if s[1] <= n < s[1] + max(s[2], 4)]
        if hit:
            s = hit[0]
            print(f"{a} -> {s[0]} @0x{s[1]:08x} size {s[2]} [{s[3]}]")
        else:
            near = sorted(syms, key=lambda s: abs(s[1] - n))[:5]
            print(f"{a} -> no exact hit; near:")
            for s in near:
                print(f"    {s[0]} @0x{s[1]:08x} size {s[2]} [{s[3]}]")

if __name__ == "__main__":
    main()
