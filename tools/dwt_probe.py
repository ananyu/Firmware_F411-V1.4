#!/usr/bin/env python3
"""dwt_probe.py - verify DWT CYCCNT is running (fsStatsInit executed)."""
import sys
import time
from pyocd.core.helpers import ConnectHelper

DEMCR = 0xE000EDFC
DWT_CTRL = 0xE0001000
DWT_CYCCNT = 0xE0001004

def main():
    sess = ConnectHelper.session_with_chosen_probe(
        target_override="stm32f411ce", connect_mode="attach")
    with sess:
        t = sess.target
        demcr = t.read32(DEMCR)
        ctrl = t.read32(DWT_CTRL)
        c1 = t.read32(DWT_CYCCNT)
        time.sleep(0.2)
        c2 = t.read32(DWT_CYCCNT)
        print("DEMCR   = 0x%08x  TRCENA=%d" % (demcr, (demcr >> 24) & 1))
        print("DWT_CTRL= 0x%08x  CYCCNTENA=%d" % (ctrl, ctrl & 1))
        print("CYCCNT  = %d -> %d  (delta=%d, expect ~19200 in 0.2s @96MHz)"
              % (c1, c2, c2 - c1))
        print(">> DWT clock RUNNING" if c2 > c1 else ">> DWT clock STOPPED")
    return 0

if __name__ == "__main__":
    sys.exit(main())
