#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
blackbox_dump.py - export/inspect the S4 flight-log blackbox over the
remoter USB CDC (ATKP passthrough, msgID DOWN/UP_BLACKBOX = 0x62).

Frame (24B, little-endian words), matches FLIGHT/src/blackbox.c:
  w0 seq u32 | w1 uptimeMs u32 | w2 roll|pitch (s16 deg x100)
  w3 yaw (s16) | vbat (u16 V x100) | w4 m1..m4 (u8 PWM 0..255)
  w5 fsCondBits u16 | fsLevel u8 | event u8 (1 arm 2 disarm 4 DEG 8 LAND 16 KILL)
  event bit 32 = slow-rate frame (10Hz cruise); otherwise full rate (100Hz).

Usage:
  python tools/blackbox_dump.py COM7 info
  python tools/blackbox_dump.py COM7 dump out.csv
  python tools/blackbox_dump.py COM7 erase
"""
import struct
import sys
import time
import serial

DOWN = 0x62
UP = 0x62
SUB_INFO, SUB_READ, SUB_ERASE = 0x01, 0x02, 0x03
UP_INFO, UP_DATA, UP_ACK = 0x01, 0x02, 0x03

EV_NAMES = {1: "ARM", 2: "DISARM", 4: "DEGRADED", 8: "LAND", 16: "KILL"}
EV_SLOW = 32  # cruise frame recorded at 10Hz (absent = full 100Hz)


def frame(sub, idx=0):
    if sub == SUB_READ:
        data = bytes([sub, idx & 0xFF, (idx >> 8) & 0xFF])
    else:
        data = bytes([sub])
    buf = bytearray([0xAA, 0xAF, DOWN, len(data)]) + data
    buf.append(sum(buf) & 0xFF)
    return bytes(buf)


class Parser:
    def __init__(self):
        self.buf = bytearray()

    def feed(self, chunk):
        self.buf += chunk
        out = []
        while True:
            i = self.buf.find(b"\xAA\xAA")
            if i < 0 or len(self.buf) - i < 4:
                if i > 0:
                    del self.buf[:i]
                break
            msgid, dlen = self.buf[i + 2], self.buf[i + 3]
            need = i + 4 + dlen + 1
            if len(self.buf) < need:
                break
            fr = self.buf[i:need]
            if (sum(fr[:-1]) & 0xFF) == fr[-1]:
                out.append((msgid, bytes(fr[4:4 + dlen])))
            del self.buf[:need]
        return out


def wait_up(s, p, want, timeout=1.0):
    """wait for an UP_BLACKBOX payload whose first byte == want"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        for msgid, pl in p.feed(s.read(4096)):
            if msgid == UP and len(pl) >= 1 and pl[0] == want:
                return pl
    return None


def get_info(s, p):
    s.write(frame(SUB_INFO)); s.flush()
    pl = wait_up(s, p, UP_INFO, 2.0)
    if not pl or len(pl) < 12:
        return None
    frames = struct.unpack_from("<I", pl, 1)[0]
    seq = struct.unpack_from("<I", pl, 5)[0]
    return frames, seq, pl[9], pl[10], pl[11]


def decode(pl):
    """UP_DATA payload -> dict (idx u16 at 1..2, 24B frame at 3)"""
    idx = struct.unpack_from("<H", pl, 1)[0]
    w = struct.unpack_from("<6I", pl, 3)
    roll = struct.unpack("<h", struct.pack("<H", w[2] & 0xFFFF))[0] / 100.0
    pitch = struct.unpack("<h", struct.pack("<H", (w[2] >> 16) & 0xFFFF))[0] / 100.0
    yaw = struct.unpack("<h", struct.pack("<H", w[3] & 0xFFFF))[0] / 100.0
    vbat = (w[3] >> 16) & 0xFFFF
    ev = (w[5] >> 24) & 0xFF
    return {
        "idx": idx, "seq": w[0], "ms": w[1],
        "roll": roll, "pitch": pitch, "yaw": yaw,
        "vbat": vbat / 100.0,
        "m1": w[4] & 0xFF, "m2": (w[4] >> 8) & 0xFF,
        "m3": (w[4] >> 16) & 0xFF, "m4": (w[4] >> 24) & 0xFF,
        "fsCond": w[5] & 0xFFFF, "fsLevel": (w[5] >> 16) & 0xFF,
        "ev": ev,
    }


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    port, mode = sys.argv[1], sys.argv[2]
    s = serial.Serial(port, 115200, timeout=0.2)
    s.reset_input_buffer()
    p = Parser()

    if mode == "info":
        info = get_info(s, p)
        if not info:
            print("[FAIL] no INFO reply (firmware too old? link down?)")
            return 1
        frames, seq, full, dropped, booted = info
        print(f"frames={frames}  seq={seq}  full={full}  dropped={dropped}  booted={booted}")
        print(f"cap 2730 frames = 10s@100Hz + ~173s@10Hz cruise (~3min); "
              f"dump for exact per-frame rates")
        return 0

    if mode == "erase":
        s.write(frame(SUB_ERASE)); s.flush()
        pl = wait_up(s, p, UP_ACK, 8.0)
        if pl and len(pl) >= 2 and pl[1] == 1:
            print("[OK] sector erased, cursor reset")
            return 0
        print("[FAIL] erase refused (armed? erase fault? no reply)")
        return 1

    if mode == "dump":
        out = sys.argv[3] if len(sys.argv) > 3 else "blackbox.csv"
        info = get_info(s, p)
        if not info:
            print("[FAIL] no INFO reply")
            return 1
        frames = info[0]
        print(f">> dumping {frames} frames...")
        rows = []
        for idx in range(frames):
            s.write(frame(SUB_READ, idx)); s.flush()
            pl = wait_up(s, p, UP_DATA, 1.5)
            if pl is None:
                print(f"[WARN] frame {idx} lost")
                continue
            rows.append(decode(pl))
        with open(out, "w", encoding="utf-8") as f:
            f.write("seq,ms,rate,roll,pitch,yaw,vbat,m1,m2,m3,m4,fsCond,fsLevel,event\n")
            for r in rows:
                ev = "|".join(EV_NAMES[b] for b in EV_NAMES if r["ev"] & b)
                rate = 10 if (r["ev"] & EV_SLOW) else 100
                f.write(f"{r['seq']},{r['ms']},{rate},{r['roll']:.2f},{r['pitch']:.2f},"
                        f"{r['yaw']:.2f},{r['vbat']:.2f},{r['m1']},{r['m2']},"
                        f"{r['m3']},{r['m4']},{r['fsCond']},{r['fsLevel']},{ev}\n")
        print(f"[OK] {len(rows)} frames -> {out}")
        nslow = sum(1 for r in rows if r["ev"] & EV_SLOW)
        print(f"     {len(rows)-nslow} full-rate(100Hz) + {nslow} cruise(10Hz)")
        evs = [r for r in rows if r["ev"] & ~EV_SLOW]
        for r in evs[:20]:
            print(f"  seq={r['seq']} ms={r['ms']} ev="
                  f"{'|'.join(EV_NAMES[b] for b in EV_NAMES if r['ev'] & b)}"
                  f" fs={r['fsLevel']}")
        return 0

    print("unknown mode:", mode)
    return 1


if __name__ == "__main__":
    sys.exit(main())
