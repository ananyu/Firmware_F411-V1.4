#!/usr/bin/env python3
# -*- coding: gbk -*-
"""
nineaxis_test.py - end-to-end nine-axis (mag) test over the remoter USB CDC.

Frame format (matches COMMUNICATE/src/atkp.c):
  uplink   : AA AA msgID len data... cksum(u8 sum of first len+4 bytes)
  downlink : AA AF msgID len data... cksum(u8 sum incl. header)
  UP_SENSER(0x02): acc x/y/z, gyro x/y/z, mag x/y/z raw (s16 BE) + pad
  UP_STATUS(0x01): roll,pitch,yaw x100 (s16 BE), alt s32, model, armed
  D_COMMAND_MAG_CALIB(0x04): toggle - 1st starts, 2nd finishes (needs >=200
  samples and flyable==false)

Usage:
  python tools/nineaxis_test.py COM7 read    # mag freshness + yaw drift, 30s
  python tools/nineaxis_test.py COM7 calib   # run mag calibration (rotate!)
"""
import struct
import sys
import time
import serial

UP_SENSER = 0x02
UP_STATUS = 0x01
DOWN_COMMAND = 0x01
D_MAG_CALIB = 0x04


def open_port(port):
    s = serial.Serial(port, 115200, timeout=0.2)
    s.reset_input_buffer()
    return s


def send_mag_calib(s):
    data = bytes([D_MAG_CALIB])
    frame = bytearray([0xAA, 0xAF, DOWN_COMMAND, len(data)]) + data
    frame.append(sum(frame) & 0xFF)
    s.write(frame)
    s.flush()


class Parser:
    """incremental ATKP uplink parser; yields (msgID, payload)"""

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
            frame = self.buf[i:need]
            if (sum(frame[:-1]) & 0xFF) == frame[-1]:
                out.append((msgid, bytes(frame[4:4 + dlen])))
            del self.buf[:need]
        return out


def s16be(b, o):
    return struct.unpack(">h", b[o:o + 2])[0]


def run(port, mode, seconds=30):
    s = open_port(port)
    p = Parser()
    t0 = time.time()
    mag_hist, yaw_hist = [], []
    cov_min = [32767] * 3
    cov_max = [-32768] * 3
    n_senser = n_status = 0
    calib_sent = False

    if mode == "calib":
        send_mag_calib(s)          # start
        print(">> 校准已开始：请拿起无人机缓慢绕各轴转动（约20秒）")
        print("   关键：机头水平转一整圈(让X轴扫过正负)+翻滚到机头朝上/朝下")
        time.sleep(0.5)

    while time.time() - t0 < seconds:
        frames = p.feed(s.read(4096))
        for msgid, payload in frames:
            if msgid == UP_SENSER and len(payload) >= 18:
                mx, my, mz = s16be(payload, 12), s16be(payload, 14), s16be(payload, 16)
                mag_hist.append((time.time() - t0, mx, my, mz))
                n_senser += 1
                if mode == "calib":
                    for k, v in enumerate((mx, my, mz)):
                        cov_min[k] = min(cov_min[k], v)
                        cov_max[k] = max(cov_max[k], v)
            elif msgid == UP_STATUS and len(payload) >= 6:
                yaw = s16be(payload, 4) / 100.0
                yaw_hist.append((time.time() - t0, yaw))
                n_status += 1
        if mode == "calib" and not calib_sent and time.time() - t0 > 20:
            send_mag_calib(s)      # finish
            calib_sent = True
            print(">> 校准结束命令已发送，等待结果...")

    s.close()

    # --- mag freshness ---
    print(f"\n[遥测] UP_SENSER帧={n_senser}  UP_STATUS帧={n_status}")
    if mode == "calib":
        names = "XYZ"
        for k in range(3):
            span = cov_max[k] - cov_min[k]
            neg = cov_min[k] < -80
            pos = cov_max[k] > 80
            flag = "OK" if (neg and pos) else "MISS"
            print(f"[覆盖] axis{names[k]}: min={cov_min[k]} max={cov_max[k]} "
                  f"span={span} 正负双向={flag}")
        print(">> 注意：原始帧轴向=传感器轴；固件内部 body 映射 X=传感器Y")
    if len(mag_hist) < 5:
        print("[FAIL] 磁力计帧太少——样本链路或门控异常")
        return 1
    ts = [t for t, _, _, _ in mag_hist]
    span = ts[-1] - ts[0]
    rate = len(mag_hist) / span if span > 0 else 0
    uniq = len(set((x, y, z) for _, x, y, z in mag_hist))
    print(f"[磁力计] 采样率={rate:.1f}Hz  不同值={uniq}/{len(mag_hist)}")
    last = mag_hist[-10:]
    mv = [m for _, m, _, _ in last]
    print(f"[磁力计] 最新 X={mv[-1]} 变化量={max(mv)-min(mv)}")

    # --- yaw drift (unwrap across +/-180 boundary) ---
    if len(yaw_hist) >= 10:
        raw = [y for _, y in yaw_hist]
        unw = [raw[0]]
        for a, b in zip(raw, raw[1:]):
            d = b - a
            while d > 180: d -= 360
            while d < -180: d += 360
            unw.append(unw[-1] + d)
        tail = unw[len(unw) * 2 // 3:]
        drift = max(tail) - min(tail)
        print(f"[yaw] 当前={raw[-1]:.2f}°  后段窗口漂移(unwrap)={drift:.2f}° / "
              f"{(yaw_hist[-1][0]-yaw_hist[len(yaw_hist)*2//3][0]):.0f}s")
        if mode == "read":
            print(">> 基线（未校准，融合yaw关闭/纯陀螺积分）已记录")

    ok = rate >= 50.0 and uniq > len(mag_hist) * 0.2
    print("\nRESULT:", "PASS" if ok else "CHECK")
    return 0 if ok else 1


if __name__ == "__main__":
    port = sys.argv[1] if len(sys.argv) > 1 else "COM7"
    mode = sys.argv[2] if len(sys.argv) > 2 else "read"
    secs = int(sys.argv[3]) if len(sys.argv) > 3 else 30
    sys.exit(run(port, mode, secs))
