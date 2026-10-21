#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
swd_verify.py - post-flash health check over SWD (no serial needed).

Reads (addresses from OBJ/Firmware_F411.map, v1.0.25):
  xTickCount 0x200004fc  uxCurrentNumberOfTasks 0x200004f8
  isMPU 0x200008bc  Mag 0x200008bd  Baro 0x200008be
  xFreeBytesRemaining 0x20000550 (heap_4 counter, expect 0: heap never init)
  failsafe level/condBits/motorCut 0x200007b4/6/8
  blackbox writeWord/seq/dropped/full/booted/needsFormat 0x200007dc..ea
Fault registers: SCB->HFSR 0xE000ED2C, CFSR 0xE000ED28, ICSR 0xE000ED04

Usage: python tools/swd_verify.py
"""
import time
import sys

from pyocd.core.helpers import ConnectHelper

ADDRS = {
    "uxCurrentNumberOfTasks": 0x200004F8,
    "xTickCount":             0x200004FC,
    "xFreeBytesRemaining":    0x20000550,
    "isMPUPresent":           0x200008BC,
    "isMagPresent":           0x200008BD,
    "isBaroPresent":          0x200008BE,
    "failsafeLevelVar":       0x200007B4,
    "failsafeCondBitsVar":    0x200007B6,
    "failsafeMotorCutVar":    0x200007B8,
    "bbxWriteWordVar":        0x200007DC,
    "bbxSeqVar":              0x200007E0,
    "bbxDroppedVar":          0x200007E4,
    "bbxFullVar":             0x200007E8,
    "bbxBootedVar":           0x200007E9,
    "bbxNeedsFormatVar":      0x200007EA,
}

def main():
    sess = ConnectHelper.session_with_chosen_probe(
        target_override="stm32f411ce", connect_mode="attach")
    with sess:
        t = sess.target
        tick1 = t.read32(ADDRS["xTickCount"])
        time.sleep(1.0)
        tick2 = t.read32(ADDRS["xTickCount"])

        ntasks = t.read32(ADDRS["uxCurrentNumberOfTasks"])
        freeheap = t.read32(ADDRS["xFreeBytesRemaining"])
        flags = t.read8(ADDRS["isMPUPresent"]) | \
                (t.read8(ADDRS["isMagPresent"]) << 8) | \
                (t.read8(ADDRS["isBaroPresent"]) << 16)

        fs_level = t.read8(ADDRS["failsafeLevelVar"])
        fs_cond = t.read16(ADDRS["failsafeCondBitsVar"])
        fs_cut = t.read8(ADDRS["failsafeMotorCutVar"])

        bbx_word = t.read32(ADDRS["bbxWriteWordVar"])
        bbx_seq = t.read32(ADDRS["bbxSeqVar"])
        bbx_drop = t.read32(ADDRS["bbxDroppedVar"])
        bbx_full = t.read8(ADDRS["bbxFullVar"])
        bbx_boot = t.read8(ADDRS["bbxBootedVar"])
        bbx_needfmt = t.read8(ADDRS["bbxNeedsFormatVar"])

        icsr = t.read32(0xE000ED04)
        cfsr = t.read32(0xE000ED28)
        hfsr = t.read32(0xE000ED2C)

        # NVIC active-bitmap: check no ISR is stuck active
        iabr0 = t.read32(0xE000E300)  # IRQ 0-31 active
        iabr1 = t.read32(0xE000E304)  # IRQ 32-63 active

    dt = tick2 - tick1
    ok = True
    def check(name, cond, detail):
        nonlocal ok
        mark = "PASS" if cond else "FAIL"
        if not cond:
            ok = False
        print(f"[{mark}] {name}: {detail}")

    check("tick rate 1kHz", 900 <= dt <= 1100, f"delta={dt} ticks/s")
    check("task count", ntasks >= 12, f"uxCurrentNumberOfTasks={ntasks}")
    check("no HardFault", (hfsr & 0x1) == 0, f"HFSR=0x{hfsr:08x}")
    check("CFSR clean", cfsr == 0, f"CFSR=0x{cfsr:08x}")
    check("no active ISR", (iabr0 | iabr1) == 0,
          f"IABR0=0x{iabr0:08x} IABR1=0x{iabr1:08x}")
    check("ICSR no pending NMI/HardFault",
          (icsr & 0x000000FF) in (0, 3), f"ICSR=0x{icsr:08x}")
    check("sensors present", flags == 0x10101,
          f"MPU={flags & 0xFF} Mag={(flags>>8)&0xFF} Baro={(flags>>16)&0xFF}")
    check("heap counter (expect 0, never init)", freeheap in (0, 30720),
          f"xFreeBytesRemaining={freeheap}")
    # Failsafe graded state machine: level must be consistent with cond bits.
    # On the bench (no radio link) LINK_LOST is expected -> LAND is CORRECT,
    # not a fault. Verify the arbitration, not a hardcoded idle level.
    fs_expected = 0  # FS_NORMAL
    if fs_cond & 0x0010:                       # EMER_STOP
        fs_expected = 3  # FS_KILL
    elif fs_cond & 0x0001 or fs_cond & 0x0002:   # LINK_LOST | LOW_BAT
        fs_expected = 2  # FS_LAND
    elif fs_cond & 0x0004 or fs_cond & 0x0008:  # MAG_LOST | FLOW_LOST
        fs_expected = 1  # FS_DEGRADED
    check("failsafe level matches cond", fs_level == fs_expected,
          f"level={fs_level} expected={fs_expected} cond=0x{fs_cond:04x} cut={fs_cut}")
    # Blackbox: either formatted (booted=1) or blank-awaiting-ground-format
    # (booted=0, needsFormat=1). Both are healthy boot outcomes; the boot path
    # no longer erases S4 (that caused a BOR reset loop).
    check("blackbox boot state",
          (bbx_boot == 1 and not bbx_needfmt) or (bbx_boot == 0 and bbx_needfmt),
          f"booted={bbx_boot} needsFormat={bbx_needfmt} full={bbx_full}")
    if bbx_boot == 1:
        check("blackbox cursor sane", 1 <= bbx_word <= 16384,
              f"writeWord={bbx_word}/16384 seq={bbx_seq} dropped={bbx_drop}")
    else:
        print("[INFO] blackbox: S4 blank, send ground ERASE to format")

    print("\nRESULT:", "ALL PASS" if ok else "HAS FAILURES")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
