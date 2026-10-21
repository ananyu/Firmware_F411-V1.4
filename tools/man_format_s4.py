"""Manually erase S4 and program BBX1 magic via pyocd flash algorithm (RAM).

If the firmware then boots with bbxBooted=1, the in-app erase path is the
faulty one; the writer/dump chain can be validated meanwhile.
"""
import struct
import sys
import time

from pyocd.core.helpers import ConnectHelper
from pyocd.flash.loader import FlashLoader

def main():
    sess = ConnectHelper.session_with_chosen_probe(
        target_override="stm32f411ce")
    with sess:
        t = sess.target
        t.halt()
        magic = struct.pack("<I", 0x42425831)
        FlashLoader.program(sess, data=magic, base_address=0x08010000,
                            erase_start=0x08010000, erase_end=0x08020000)
        t.reset()
        t.resume()
        time.sleep(3.0)
        w0 = t.read32(0x08010000)
        tick = t.read32(0x200004FC)
        tasks = t.read32(0x200004F8)
        booted = t.read8(0x200007E9)
        word = t.read32(0x200007DC)
        seq = t.read32(0x200007E0)
        print("S4 word0=0x%08x tick=%d tasks=%d bbxBooted=%d "
              "bbxWriteWord=%d bbxSeq=%d" % (w0, tick, tasks, booted, word, seq))
    return 0

if __name__ == "__main__":
    sys.exit(main())
