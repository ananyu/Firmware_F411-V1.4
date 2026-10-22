#!/usr/bin/env python3
"""fs_stats_capture.py - reset FC, capture COM7, reassemble UP_PRINTF ATKP frames.

Console printf is chunked into <=30-byte ATKP uplink frames
(AA AA msgID=0x51 len data... cksum). This joins the payloads back into
readable text so the [FS_STATS] table can be parsed for CPU% / stackFree.
"""
import subprocess
import sys
import time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM7"
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 13.0
UP_PRINTF = 0x51

def extract_text(buf):
    """COM7 carries two interleaved things: raw console printf bytes (the FS
    table) and binary ATKP telemetry frames (AA AA msgid len data cksum).
    Drop whole well-formed ATKP frames so their printable-range bytes don't
    pollute the text; the leftover bytes are the raw console stream."""
    out = bytearray()
    i = 0
    n = len(buf)
    while i < n:
        if buf[i] == 0xAA and i + 3 < n and buf[i + 1] == 0xAA:
            dlen = buf[i + 3]
            total = dlen + 5
            # only treat as a frame if the byte after it is a plausible msgid
            # and the frame fits; otherwise fall through and keep the byte.
            if 0 < dlen <= 0x40 and i + total <= n:
                i += total
                continue
        out.append(buf[i])
        i += 1
    keep = bytearray()
    for b in out:
        if b in (0x0A, 0x0D) or 0x20 <= b <= 0x7E:
            keep.append(b)
    return keep.decode("ascii", errors="replace")

def main():
    s = serial.Serial(PORT, 115200, timeout=0.1)
    s.reset_input_buffer()
    subprocess.run(["pyocd", "reset", "-t", "stm32f411ce"],
                   capture_output=True, text=True)
    t0 = time.time()
    buf = bytearray()
    while time.time() - t0 < SECS:
        buf += s.read(4096)
    s.close()
    text = extract_text(buf)
    out = sys.argv[3] if len(sys.argv) > 3 else None
    if out:
        with open(out, "w", encoding="utf-8") as f:
            f.write(text)
        print("saved %d chars -> %s" % (len(text), out))
    lines = text.splitlines()
    shown = 0
    for ln in lines:
        if "STATS" in ln or "stackFree" in ln or "pri" in ln:
            print(ln)
            shown += 1
    # also print the data rows (task name + numbers)
    import re
    for ln in lines:
        if re.search(r"\d+%\s+\d+", ln):
            print(ln)
            shown += 1
    print("--- %d raw bytes, %d matched lines ---" % (len(buf), shown))

if __name__ == "__main__":
    sys.exit(main())
