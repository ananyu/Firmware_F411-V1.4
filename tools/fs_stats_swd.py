#!/usr/bin/env python3
"""fs_stats_swd.py - read the static s_taskStatus[] snapshot over SWD.

fs_stats.c fills TaskStatus_t s_taskStatus[20] every 5s via
uxTaskGetSystemState(). Reading it directly gives an exact, un-fragmented
name <-> runTime <-> stackHighWater table (the console printf path chunks
each line into <=30-byte ATKP frames and mangles the task names).

TaskStatus_t layout (ARM 32-bit, packed to 4-byte alignment):
  +0  xHandle              (u32)
  +4  pcTaskName           (ptr)
  +8  xTaskNumber          (u32)
  +12 eCurrentState        (u32)
  +16 uxCurrentPriority    (u32)
  +20 uxBasePriority       (u32)
  +24 ulRunTimeCounter     (u32)
  +28 pxStackBase          (ptr)
  +32 usStackHighWaterMark (u16) [+2 pad]
  size = 36 bytes
"""
import sys
from pyocd.core.helpers import ConnectHelper

ARR = 0x2000c3c4
ENT = 36
MAX = 20

def read_cstr(t, addr, limit=24):
    out = bytearray()
    for i in range(limit):
        c = t.read8(addr + i)
        if c == 0:
            break
        out.append(c)
    return out.decode("ascii", "replace")

def main():
    sess = ConnectHelper.session_with_chosen_probe(
        target_override="stm32f411ce", connect_mode="attach")
    with sess:
        t = sess.target
        rows = []
        total_rt = 0
        for k in range(MAX):
            base = ARR + k * ENT
            handle = t.read32(base)
            if handle == 0:
                continue
            name_ptr = t.read32(base + 4)
            pri = t.read32(base + 16)
            rt = t.read32(base + 24)
            hw = t.read16(base + 32)
            name = read_cstr(t, name_ptr)
            rows.append((name, pri, rt, hw))
            total_rt += rt
        print("%-14s %3s %8s %6s %6s" % ("name", "pri", "runtime", "cpu%", "stkFree"))
        for name, pri, rt, hw in sorted(rows, key=lambda r: -r[2]):
            pct = (rt * 100.0 / total_rt) if total_rt else 0.0
            print("%-14s %3d %8d %5.1f%% %6d" % (name, pri, rt, pct, hw))
        print("tasks=%d total_runtime=%d" % (len(rows), total_rt))
    return 0

if __name__ == "__main__":
    sys.exit(main())
