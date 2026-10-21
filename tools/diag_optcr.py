"""One-shot SWD read: FLASH regs + option-byte write protection + bbx vars."""
import sys
from pyocd.core.helpers import ConnectHelper

def main():
    sess = ConnectHelper.session_with_chosen_probe(
        target_override="stm32f411ce", connect_mode="attach")
    with sess:
        t = sess.target
        rd = lambda a: t.read32(a)
        FB = 0x40023C00
        acr = rd(FB + 0x00)
        sr = rd(FB + 0x0C)
        cr = rd(FB + 0x10)
        optcr = rd(FB + 0x14)
        print("FLASH ACR   = 0x%08x" % acr)
        print("FLASH SR    = 0x%08x  BSY=%d ERRbits=0x%02x" % (sr, (sr >> 16) & 1, sr & 0xF2))
        print("FLASH CR    = 0x%08x  LOCK=%d PG=%d SER=%d PSIZE=%d" %
              (cr, cr & 1, (cr >> 1) & 1, (cr >> 3) & 1, (cr >> 8) & 3))
        print("FLASH_OPTCR = 0x%08x  OPTLOCK=%d nWRP=0x%02x" %
              (optcr, optcr & 1, (optcr >> 16) & 0xFF))
        print("RCC_CSR     = 0x%08x" % rd(0x40023874))
        print("S4 word0    = 0x%08x" % rd(0x08010000))
        print("S4 word1    = 0x%08x" % rd(0x08010004))
        for name, addr in [("bbxWriteWord", 0x200007DC), ("bbxSeq", 0x200007E0),
                           ("bbxDropped", 0x200007E4), ("bbxFull", 0x200007E8),
                           ("bbxBooted", 0x200007E9), ("xTickCount", 0x200004FC),
                           ("uxTasks", 0x200004F8)]:
            print("%-14s = 0x%08x" % (name, rd(addr)))
    return 0

if __name__ == "__main__":
    sys.exit(main())
