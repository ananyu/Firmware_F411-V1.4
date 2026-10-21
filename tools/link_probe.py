#!/usr/bin/env python3
"""link_probe.py - read commander link/lock state over SWD (v1.0.25 map)."""
import sys
from pyocd.core.helpers import ConnectHelper

A = {"isRCLocked": 0x200006F0, "tick": 0x200004FC,
     "fsLevel": 0x200007B4, "fsCond": 0x200007B6}

def main():
    sess = ConnectHelper.session_with_chosen_probe(
        target_override="stm32f411ce", connect_mode="attach")
    with sess:
        t = sess.target
        print("isRCLocked   =", t.read8(A["isRCLocked"]), "(1=no link)")
        print("xTickCount   =", t.read32(A["tick"]))
        print("failsafeLvl  =", t.read8(A["fsLevel"]))
        print("failsafeCond = 0x%04x" % t.read16(A["fsCond"]))
    return 0

if __name__ == "__main__":
    sys.exit(main())
