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

Transfer mode: sliding-window (go-back-N) DATA pipeline.
  The flight side processes frames strictly in order and answers every
  frame with a cumulative watermark:
    ST_OK         value = offset after append   (progress)
    ST_ERR_OFFSET value = offset it expects     (gap detected, resync)
  The sender keeps WINDOW frames in flight and needs no per-frame stop-and-
  wait, which removes one full radio round-trip (~220 ms) from every 24-byte
  chunk.  Both ACK values report the same firmware-side otaRxOffset, so:
    ERR value >  acked : we missed OKs, jump forward
    ERR value == acked : gap at watermark, resend from there (no-op if
                         nothing is in flight -- also covers delayed dupes)
    ERR value <  acked : stale rejection of an old duplicate, ignore
  Firmware without this handling still works: it answers out-of-order
  frames with ERR_OFFSET(expected), which is exactly the resync path above.

Usage:
  python ota_upload.py COM3 USER\\BIN\\Firmware_F411_image.bin
  python ota_upload.py COM3 image.bin --baud 115200 --window 8

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

CHUNK = 22          # DATA chunk per atkp frame (30 - sub/offset/len/crc16)
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


def crc16(data):
    """CRC16-CCITT (init 0xFFFF, poly 0x1021) - matches otaCrc16() in ota.c."""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


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


def cmd_status(ser):
    """Query firmware-side rx offset. Returns offset (>=0) or None if no ACK.
    offset==0 means the state machine is NOT in DOWNLOAD (idle / after BEGIN)."""
    reader = AckReader(ser)
    for _ in range(3):
        ser.write(build_frame(DOWN_OTA, bytes([SUB_STATUS])))
        st, val = reader.wait_ack(SUB_STATUS, timeout=2.0)
        if st is not None and val is not None:
            return val
    return None


def cmd_data(ser, image, window):
    """Sliding-window DATA transfer (go-back-N on cumulative watermark)."""
    reader = AckReader(ser)
    total = len(image)
    acked = 0           # cumulative watermark confirmed by the firmware
    sent = 0            # next offset to transmit
    resyncs = 0
    idle_strikes = 0    # consecutive "ERR_STATE + STATUS==0" confirmations
    t0 = time.time()
    last_print = 0.0

    while acked < total:
        # Fill the window: keep up to `window` chunks in flight.
        while sent < total and sent - acked < window * CHUNK:
            chunk = image[sent:sent + CHUNK]
            body = struct.pack("<I", sent) + bytes([len(chunk)]) + chunk
            # frame CRC16 over offset||len||chunk: the atkp u8 sum lets
            # parser-desync garbage through 1/256 of the time and the
            # firmware would program it into slot1 silently.
            payload = bytes([SUB_DATA]) + body + struct.pack("<H", crc16(body))
            ser.write(build_frame(DOWN_OTA, payload))
            sent += len(chunk)

        st, val = reader.wait_ack(SUB_DATA, timeout=ACK_TIMEOUT)
        if st == ST_OK:
            idle_strikes = 0
            if val is not None and val > acked:
                acked = val
                sent = max(sent, acked)  # 不重发水位线之前的数据
        elif st == ST_ERR_OFFSET and val is not None:
            idle_strikes = 0
            if val > acked:
                # Missed OKs: firmware is further along than our watermark.
                acked = val
                sent = max(sent, acked)
            elif val == acked:
                # Gap at the watermark: resend from there. No-op when
                # nothing is in flight; also absorbs stale duplicate rejects.
                if sent > acked:
                    resyncs += 1
                sent = acked
            # val < acked: stale rejection of an old duplicate -> ignore
        elif st == ST_ERR_STATE:
            # ERR_STATE 可能是遥测流字节被误解析出的假ACK，也可能是固件
            # 状态机真的丢失了 DOWNLOAD 状态。用 STATUS 查询甄别。
            real = cmd_status(ser)
            if real is None:
                continue  # 无法确认，保持现状继续等（可能只是链路抖动）
            if real == 0:
                # STATUS 报 0：可能是假ACK后的空状态，也可能真丢了。连续
                # 3 次无进展就认定为真丢，交给上层重新 BEGIN。
                idle_strikes += 1
                if idle_strikes >= 3 or acked > 0:
                    return False
            else:
                idle_strikes = 0
                if real > acked:
                    acked = real
                    sent = max(sent, acked)
        elif st is None:
            # No progress within ACK_TIMEOUT: go back to the watermark.
            if sent > acked:
                resyncs += 1
            sent = acked
        else:
            # Transient error statuses: keep waiting for real progress.
            time.sleep(0.02)

        now = time.time()
        if now - last_print >= 0.5:
            last_print = now
            done = acked * 100 // total
            sys.stdout.write("\r  %d%% (%d/%d bytes)" % (done, acked, total))
            sys.stdout.flush()

    sys.stdout.write("\n")
    dt = time.time() - t0
    print("DATA done in %.1f s (%.0f B/s), resyncs: %d" %
          (dt, total / max(dt, 0.1), resyncs))
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
    ap.add_argument("--window", type=int, default=8,
                    help="frames in flight during DATA (default 8)")
    args = ap.parse_args()

    with open(args.image, "rb") as f:
        image = f.read()

    if len(image) > 0x1FC00:
        sys.exit("image too large: %d bytes (max 0x1FC00)" % len(image))

    ser = serial.Serial(args.port, args.baud, timeout=0, write_timeout=2)
    try:
        print("Port %s @ %d, image %d bytes, window %d" %
              (args.port, args.baud, len(image), args.window))
        print("Note: aircraft must be DISARMED or BEGIN is refused (ERR_ARMED).")
        ok = False
        for attempt in range(2):
            if not cmd_begin(ser, len(image)):
                sys.exit(2)
            if cmd_data(ser, image, args.window):
                ok = True
                break
            print("firmware OTA state lost (confirmed via STATUS); "
                  "restarting transfer from scratch")
        if not ok:
            sys.exit(3)
        if not cmd_end(ser):
            sys.exit(4)
        time.sleep(3)  # let the FC reset and the bootloader swap
    finally:
        ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
