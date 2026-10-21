#!/usr/bin/env python3
"""bbx_forensics.py - read the v1.0.23 blackbox boot-failure snapshots.

bbxStageVar: 0=never ran, 1=stalled in erase, 2=stalled at magic program,
3=format done then crashed?, 4=scan path done. bbxBootedVar=1 means ok.
"""
import sys
import time

from pyocd.core.helpers import ConnectHelper

A = [
    ("tick",        0x200004FC, 4),
    ("tasks",       0x200004F8, 4),
    ("bbxBooted",   0x200007E9, 1),
    ("bbxNeedFmt",  0x200007EA, 1),
    ("bbxStage",    0x200007EB, 1),
    ("bbxMagicRead",0x200007EC, 4),
    ("bbxEraseSr",  0x200007F0, 4),
    ("bbxEraseW0",  0x200007F4, 4),
    ("bbxProgSr",   0x200007F8, 4),
    ("bbxProgW0",   0x200007FC, 4),
    ("bbxScanEnd",  0x20000800, 4),
    ("bbxWriteWord",0x200007DC, 4),
    ("bbxSeq",      0x200007E0, 4),
    ("bbxFull",     0x200007E8, 1),
    ("S4word0",     0x08010000, 4),
    ("RCC_CSR",     0x40023874, 4),
]

def main():
    sess = ConnectHelper.session_with_chosen_probe(
        target_override="stm32f411ce", connect_mode="attach")
    with sess:
        t = sess.target
        for name, addr, sz in A:
            v = (t.read32(addr) if sz == 4 else t.read8(addr))
            print("%-13s = 0x%08x (%d)" % (name, v, v))
    return 0

if __name__ == "__main__":
    sys.exit(main())
