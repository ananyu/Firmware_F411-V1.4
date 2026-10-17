#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
bin2hex.py - Convert a raw binary file to Intel HEX format with a base address.

Usage:
    python bin2hex.py <input.bin> <output.hex> <base_address>

Example:
    python bin2hex.py Firmware_F411_image.bin Firmware_F411_image.hex 0x08020000
"""

import sys


def write_record(f, rectype, addr, data):
    rec = bytearray()
    rec.append(len(data))
    rec.append((addr >> 8) & 0xFF)
    rec.append(addr & 0xFF)
    rec.append(rectype)
    rec.extend(data)
    cksum = (-sum(rec)) & 0xFF
    rec.append(cksum)
    f.write(":" + rec.hex().upper() + "\r\n")


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 1

    src, dst, base_s = sys.argv[1], sys.argv[2], sys.argv[3]
    base = int(base_s, 16)

    with open(src, "rb") as f:
        blob = f.read()

    with open(dst, "w", newline="") as f:
        # Extended linear address record (type 04)
        uba = (base >> 16) & 0xFFFF
        if uba:
            write_record(f, 0x04, 0, uba.to_bytes(2, "big"))

        addr = base & 0xFFFF
        offset = 0
        while offset < len(blob):
            chunk = blob[offset:offset + 16]
            write_record(f, 0x00, addr, chunk)
            addr = (addr + len(chunk)) & 0xFFFF
            offset += len(chunk)
            if addr == 0 and offset < len(blob):
                uba += 1
                write_record(f, 0x04, 0, uba.to_bytes(2, "big"))

        write_record(f, 0x01, 0, b"")  # EOF

    print("OK: %s (base=0x%08X, %d bytes)" % (dst, base, len(blob)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
