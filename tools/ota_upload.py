#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
ota_upload.py - upload a signed MCUBoot image to MiniFly over the USART1
OTA bridge (or any transparent link carrying atkp frames, e.g. a USB-TTL
adapter wired to PA9/PA10).

Frame format (matches COMMUNICATE/src/ota.c):
  downlink : AA AF 0x60 len data... cksum      (cksum = u8 sum of all bytes)
  uplink   : AA AA 0x61 len data... cksum
  data[0]=sub-command, data[1]=status, data[2..5]=u32 LE value (rx offset)

Usage:
  python ota_upload.py COM3 USER\\BIN\\Firmware_F411_image.bin
  python ota_upload.py COM3 image.bin --baud 115200

Requires pyserial:  pip install pyserial
"""
import argparse
import struct
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial not found. Install it with:  pip install pyserial")

DOWN_BYTE1, DOWN_BYTE2 = 0xAA, 0xAF
UP_BYTE1, UP_BYTE2 = 0xAA, 0xAA
DOWN_OTA = 0x60
UP_OTA = 0x61

SUB_STATUS = 0x00
SUB_BEGIN = 0x01
SUB_DATA = 0x02
SUB_END = 0x03

ST_OK = 0
ST_ERR_ARMED = 1
ST_ERR_SIZE = 2
ST_ERR_OFFSET = 3
ST_ERR_VERIFY = 4
ST_ERR_STATE = 5

CHUNK = 24          # max DATA payload per atkp frame (30 - 6 header bytes)
ACK_TIMEOUT = 3.0   # seconds; BEGIN 含 S6 扇区擦除，ACK 回环可能超过 1s
MAX_RETRY = 8

ST_NAMES = {
    ST_OK: "OK",
    ST_ERR_ARMED: "ERR_ARMED",
    ST_ERR_SIZE: "ERR_SIZE",
    ST_ERR_OFFSET: "ERR_OFFSET",
    ST_ERR_VERIFY: "ERR_VERIFY",
    ST_ERR_STATE: "ERR_STATE",
}


def build_frame(msg_id, payload):
    body = bytes([DOWN_BYTE1, DOWN_BYTE2, msg_id, len(payload)]) + payload
    cksum = sum(body) & 0xFF
    return body + bytes([cksum])


class AckReader(object):
    """Incremental parser for uplink atkp frames; returns (sub, status, value)."""

    def __init__(self, ser):
        self.ser = ser
        self.buf = bytearray()

    def _drain(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            n = self.ser.in_waiting
            if n:
                self.buf.extend(self.ser.read(n))
                # keep the buffer bounded but never split a frame tail
                if len(self.buf) > 256:
                    self.buf = self.buf[-64:]
            else:
                time.sleep(0.002)

    def wait_ack(self, sub, timeout=ACK_TIMEOUT):
        """Wait for an UP_OTA ACK whose data[0] == sub. Returns (status, value)."""
        end = time.time() + timeout
        while time.time() < end:
            # 小切片轮询：收一点就解析，避免高码率遥测流把未解析的ACK冲掉(_drain内有截断)
            self._drain(min(0.02, max(0.0, end - time.time())))
            # resync search
            while True:
                i = self.buf.find(bytes([UP_BYTE1, UP_BYTE2, UP_OTA]))
                if i < 0:
                    if len(self.buf) > 2:
                        self.buf = self.buf[-2:]
                    break
                if i > 0:
                    self.buf = self.buf[i:]
                if len(self.buf) < 4:
                    break
                dlen = self.buf[3]
                total = 4 + dlen + 1
                if len(self.buf) < total:
                    break
                frame = self.buf[:total]
                self.buf = self.buf[total:]
                if (sum(frame[:-1]) & 0xFF) != frame[-1]:  # 累加和校验：末字节=前面所有字节之和
                    continue  # bad checksum, keep resyncing
                data = frame[4:4 + dlen]
                if len(data) < 6 or data[0] != sub:
                    continue
                value = struct.unpack("<I", data[2:6])[0]
                return data[1], value
        return None, None


def send_ota(ser, payload, expect_sub):
    ser.write(build_frame(DOWN_OTA, payload))
    return ser, expect_sub


def cmd_begin(ser, total):
    payload = bytes([SUB_BEGIN]) + struct.pack("<I", total)
    for attempt in range(MAX_RETRY):
        ser.write(build_frame(DOWN_OTA, payload))
        st, val = AckReader(ser).wait_ack(SUB_BEGIN)
        if st == ST_OK:
            print("BEGIN ok, image size %d bytes" % total)
            return True
        if st is None:
            print("BEGIN: no ACK, retry %d" % (attempt + 1))
            continue
        print("BEGIN failed: %s (0x%02x)" % (ST_NAMES.get(st, "?"), st))
        return False
    print("BEGIN: no ACK after %d tries" % MAX_RETRY)
    return False


def cmd_data(ser, image):
    reader = AckReader(ser)
    offset = 0
    total = len(image)
    t0 = time.time()
    while offset < total:
        chunk = image[offset:offset + CHUNK]
        payload = bytes([SUB_DATA]) + struct.pack("<I", offset) + \
            bytes([len(chunk)]) + chunk
        ser.write(build_frame(DOWN_OTA, payload))
        st, val = reader.wait_ack(SUB_DATA)
        if st == ST_OK:
            offset = val  # firmware reports the accepted offset
        elif st == ST_ERR_OFFSET and val is not None:
            print("resync at offset %d" % val)
            offset = val
        else:
            print("DATA @0x%x failed: %s, retrying" %
                  (offset, ST_NAMES.get(st, "NO_ACK")))
            time.sleep(0.05)
            continue
        done = offset * 100 // total
        sys.stdout.write("\r  %d%% (%d/%d bytes)" % (done, offset, total))
        sys.stdout.flush()
    sys.stdout.write("\n")
    dt = time.time() - t0
    print("DATA done in %.1f s (%.0f B/s)" % (dt, total / max(dt, 0.1)))
    return True


def cmd_end(ser):
    reader = AckReader(ser)
    for attempt in range(MAX_RETRY):
        ser.write(build_frame(DOWN_OTA, bytes([SUB_END])))
        st, val = reader.wait_ack(SUB_END, timeout=max(ACK_TIMEOUT, 5.0))
        if st == ST_OK:
            print("END ok: image verified, swap marked pending.")
            print("The flight controller resets into MCUBoot now; do not power off.")
            return True
        if st is None:
            print("END: no ACK, retry %d" % (attempt + 1))
            continue
        print("END failed: %s (offset %d)" % (ST_NAMES.get(st, "?"), val or 0))
        return False
    return False


def main():
    ap = argparse.ArgumentParser(description="MiniFly OTA upload (atkp/OTA over UART)")
    ap.add_argument("port", help="serial port, e.g. COM3")
    ap.add_argument("image", help="signed image file, e.g. USER\\BIN\\Firmware_F411_image.bin")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    with open(args.image, "rb") as f:
        image = f.read()

    if len(image) > 0x1FC00:
        sys.exit("image too large: %d bytes (max 0x1FC00)" % len(image))

    ser = serial.Serial(args.port, args.baud, timeout=0, write_timeout=2)
    try:
        print("Port %s @ %d, image %d bytes" % (args.port, args.baud, len(image)))
        print("Note: aircraft must be DISARMED or BEGIN is refused (ERR_ARMED).")
        if not cmd_begin(ser, len(image)):
            sys.exit(2)
        if not cmd_data(ser, image):
            sys.exit(3)
        if not cmd_end(ser):
            sys.exit(4)
        time.sleep(3)  # let the FC reset and the bootloader swap
    finally:
        ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
