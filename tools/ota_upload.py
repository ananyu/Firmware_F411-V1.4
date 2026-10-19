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
  python ota_upload.py COM3 new.bin --diff old.bin   (differential; auto
      falls back to a full upload when the running baseline mismatches)

Requires pyserial:  pip install pyserial
"""
import argparse
import struct
import sys
import time
import zlib

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
SUB_DBEGIN = 0x04
SUB_DDATA = 0x05
SUB_DEND = 0x06

ST_OK = 0
ST_ERR_ARMED = 1
ST_ERR_SIZE = 2
ST_ERR_OFFSET = 3
ST_ERR_VERIFY = 4
ST_ERR_STATE = 5

CHUNK = 22          # DATA chunk per atkp frame (30 - sub/offset/len/crc16)
ACK_TIMEOUT = 3.0   # seconds; BEGIN 含 S6 扇区擦除，ACK 回环可能超过 1s
MAX_RETRY = 8

# --- differential patch format (matches otaApplyPatch() in ota.c) ---
OP_COPY = 0x01      # 0x01 + u32 src_off + u16 n : copy old[src_off:src_off+n]
OP_LIT = 0x02       # 0x02 + u8 n + n bytes      : literal (n 1..255)
OP_END = 0xFF
BLOCK = 16          # match-index block size
MIN_MATCH = 12      # COPY costs 7 bytes -> pays off from n >= 12
MAX_COPY = 0xFFFF   # u16 length field

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


# ---- differential patch generator -------------------------------------
# Greedy longest-match against a 16-byte block hash index over the old
# image. Firmware images are highly repetitive, so a version bump usually
# collapses to a few hundred bytes of patch instead of a ~110 KB upload
# over a ~108 B/s radio link.

def _extend_match(new, old, j, pos, k):
    """Extend a k-byte match at new[j] vs old[pos] forward, in 64-byte
    chunks for speed, capped at MAX_COPY."""
    n, o = len(new), len(old)
    while k < MAX_COPY and j + k < n and pos + k < o:
        step = min(64, MAX_COPY - k, n - j - k, o - pos - k)
        if new[j + k:j + k + step] == old[pos + k:pos + k + step]:
            k += step
        else:
            break
    return k


def _flush_lit(out, lit):
    i = 0
    while i < len(lit):
        n = min(255, len(lit) - i)
        out += bytes([OP_LIT, n]) + lit[i:i + n]
        i += n
    del lit[:]


def make_patch(old, new):
    """Encode `old` -> `new` as COPY/LIT/END instructions."""
    index = {}
    for i in range(0, len(old) - BLOCK + 1):
        index.setdefault(old[i:i + BLOCK], []).append(i)

    out = bytearray()
    lit = bytearray()
    j = 0
    n = len(new)
    while j < n:
        cands = index.get(new[j:j + BLOCK])
        best = None
        if cands:
            for pos in cands:
                k = _extend_match(new, old, j, pos, BLOCK)
                if best is None or k > best[1]:
                    best = (pos, k)
        if best is not None and best[1] >= MIN_MATCH:
            _flush_lit(out, lit)
            out += bytes([OP_COPY])
            out += struct.pack("<I", best[0])
            out += struct.pack("<H", best[1])
            j += best[1]
        else:
            lit.append(new[j])
            j += 1
            if len(lit) >= 255:
                _flush_lit(out, lit)
    _flush_lit(out, lit)
    out.append(OP_END)
    return bytes(out)


def apply_patch(old, patch, new_size):
    """Offline interpreter mirroring otaApplyPatch(); used to self-test the
    generated patch before any bytes touch the air."""
    out = bytearray()
    i = 0
    while i < len(patch):
        op = patch[i]
        i += 1
        if op == OP_END:
            break
        if op == OP_COPY:
            src, n = struct.unpack_from("<IH", patch, i)
            i += 6
            if src + n > len(old):
                raise ValueError("COPY out of bounds at %d" % (i - 7))
            out += old[src:src + n]
        elif op == OP_LIT:
            n = patch[i]
            i += 1
            out += patch[i:i + n]
            i += n
        else:
            raise ValueError("bad opcode 0x%02x at %d" % (op, i - 1))
    if len(out) != new_size:
        raise ValueError("rebuilt size %d != %d" % (len(out), new_size))
    return bytes(out)


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


def cmd_data(ser, image, window, sub=SUB_DATA):
    """Sliding-window DATA transfer (go-back-N on cumulative watermark).
    sub selects the downlink payload id (SUB_DATA -> slot1, SUB_DDATA ->
    scratch); the firmware ACKs both with SUB_DATA."""
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
            payload = bytes([sub]) + body + struct.pack("<H", crc16(body))
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


def cmd_dbegin(ser, patch_size, old_size, new_size, old_crc):
    """DBEGIN: check baseline CRC32, erase scratch, arm the patch state.
    Any rejection (incl. ERR_VERIFY = running image != OLD_IMAGE) makes the
    caller fall back to a full upload."""
    payload = bytes([SUB_DBEGIN]) + \
        struct.pack("<IIII", patch_size, old_size, new_size, old_crc)
    reader = AckReader(ser)
    for attempt in range(MAX_RETRY):
        ser.write(build_frame(DOWN_OTA, payload))
        # scratch S7 (128 KB) sector erase takes ~2-4 s
        st, val = reader.wait_ack(SUB_DBEGIN, timeout=max(ACK_TIMEOUT, 6.0))
        if st == ST_OK:
            print("DBEGIN ok: baseline match (CRC32 %08x), patch %d bytes" %
                  (old_crc, patch_size))
            return True
        if st is None:
            print("DBEGIN: no ACK, retry %d" % (attempt + 1))
            continue
        print("DBEGIN failed: %s (0x%02x)" % (ST_NAMES.get(st, "?"), st))
        if st == ST_ERR_VERIFY:
            print("baseline mismatch: running image differs from OLD_IMAGE")
        return False
    return False


def wait_reboot(ser, reader, window=45.0):
    """Watch the OTA link around the DEND reboot: STATUS must stop being
    answered (slot1 rebuild + reset, link silent by design) and then answer
    again once the new image has booted."""
    t0 = time.time()
    deadline = t0 + window
    quiet = None            # when the link went silent
    while time.time() < deadline:
        ser.write(build_frame(DOWN_OTA, bytes([SUB_STATUS])))
        st, val = reader.wait_ack(SUB_STATUS, timeout=1.5)
        if st is None:
            if quiet is None:
                quiet = time.time()
                print("link silent: device is resetting into MCUBoot...")
        elif quiet is not None:
            if time.time() - quiet >= 2.0:
                print("device back up (STATUS=%d) after %.1f s of silence."
                      % (val, time.time() - quiet))
                return True
            quiet = None    # short glitch (radio drop), keep watching
        else:
            # The post-ACK rebuild never answers (the OTA task is busy and
            # queued frames die with the reset), so a link that stays
            # responsive for >12 s means the rebuild failed early and sent
            # a follow-up error ACK instead of rebooting.
            if time.time() - t0 > 12.0:
                print("device never went silent: DEND work failed after the ACK.")
                return False
    print("timeout waiting for the reboot")
    return False


def cmd_dend(ser):
    """DEND: ACK-first protocol. The firmware ACKs OK immediately, then
    erases + rebuilds slot1 (link silent for several seconds), verifies,
    marks the swap pending and resets. A second ACK after the rebuild would
    always miss the host timeout, so success is judged by watching the link
    go silent and come back (wait_reboot)."""
    reader = AckReader(ser)
    for attempt in range(MAX_RETRY):
        ser.write(build_frame(DOWN_OTA, bytes([SUB_DEND])))
        st, val = reader.wait_ack(SUB_DEND, timeout=max(ACK_TIMEOUT, 6.0))
        if st is None:
            print("DEND: no ACK, retry %d" % (attempt + 1))
            continue
        if st != ST_OK:
            print("DEND failed: %s" % ST_NAMES.get(st, "?"))
            return False
        break
    else:
        print("DEND: no ACK after %d tries" % MAX_RETRY)
        return False
    print("DEND accepted (image size %d): rebuilding slot1 now." % val)
    print("The link is silent while slot1 is erased+rebuilt, then the FC "
          "resets into MCUBoot. Do not power off.")
    return wait_reboot(ser, reader)


def main():
    ap = argparse.ArgumentParser(description="MiniFly OTA upload (atkp/OTA over UART)")
    ap.add_argument("port", help="serial port, e.g. COM3")
    ap.add_argument("image", help="signed image file, e.g. USER\\BIN\\Firmware_F411_image.bin")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--window", type=int, default=8,
                    help="frames in flight during DATA (default 8)")
    ap.add_argument("--diff", metavar="OLD_IMAGE",
                    help="upload differentially: generate a patch against "
                         "OLD_IMAGE (the image currently running on the "
                         "aircraft) and fall back to a full upload when the "
                         "baseline does not match")
    args = ap.parse_args()

    with open(args.image, "rb") as f:
        image = f.read()

    if len(image) > 0x1FC00:
        sys.exit("image too large: %d bytes (max 0x1FC00)" % len(image))

    patch = None
    old = None
    if args.diff:
        with open(args.diff, "rb") as f:
            old = f.read()
        patch = make_patch(old, image)
        # never ship a patch that is not cheaper than the full image
        if len(patch) >= len(image):
            print("patch (%d bytes) >= full image; uploading full instead" %
                  len(patch))
            patch = None
        else:
            try:
                if apply_patch(old, patch, len(image)) != image:
                    raise ValueError("rebuilt image differs")
            except ValueError as e:
                sys.exit("patch self-test failed: %s" % e)
            print("patch: %d bytes vs %d full (%.1f%%) - self-test passed" %
                  (len(patch), len(image), 100.0 * len(patch) / len(image)))

    ser = serial.Serial(args.port, args.baud, timeout=0, write_timeout=2)
    try:
        print("Port %s @ %d, image %d bytes, window %d" %
              (args.port, args.baud, len(image), args.window))
        print("Note: aircraft must be DISARMED or BEGIN is refused (ERR_ARMED).")
        ok = False
        if patch is not None:
            for attempt in range(2):
                if not cmd_dbegin(ser, len(patch), len(old), len(image),
                                  zlib.crc32(old) & 0xFFFFFFFF):
                    break  # baseline mismatch / rejected -> full upload
                if cmd_data(ser, patch, args.window, sub=SUB_DDATA) \
                        and cmd_dend(ser):
                    ok = True
                    break
                print("diff transfer failed; falling back to full upload")
                ser.reset_input_buffer()
        if not ok:
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
