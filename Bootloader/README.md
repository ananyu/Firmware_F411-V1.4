# Bootloader — MCUboot port for MiniFly STM32F411CE

This directory contains **only the MCUboot porting layer** written for the
MiniFly flight controller (STM32F411CEU6, 512 KB internal flash). The secure
boot core — `boot_go()`, swap state machine, image validation, TLV handling —
is the unmodified upstream [MCUboot **v2.1.0**](https://github.com/mcu-tools/mcuboot)
(Apache-2.0), compiled directly from its source tree.

## Directory layout

```
Bootloader/
├── main.c                      # boot entry: boot_go() + jump_to_app() + LED status
├── flash_map_backend/
│   ├── flash_map_backend.c     # flash_area API over STM32F4 FWLIB (word program, sector erase)
│   └── flash_map_backend.h     # flash_area / flash_sector structs (Zephyr-less contract)
├── mcuboot_config/
│   ├── mcuboot_config.h        # feature switches + AC5 compiler fix-ups (_Static_assert shim)
│   └── mcuboot_logging.h       # no-op logging stub (saves flash)
├── os/
│   └── os_malloc.h             # stdlib mapping stub (loader.c includes it unconditionally)
└── sysflash/
    └── sysflash.h              # flash area IDs for the partition table below
```

## Flash partition table (STM32F411CE, 512 KB)

| Region            | Address      | Size  | Sector |
|-------------------|--------------|-------|--------|
| Bootloader        | `0x08000000` | 128K  | S0–S4  |
| Image primary     | `0x08020000` | 128K  | S5     |
| Image secondary   | `0x08040000` | 128K  | S6     |
| Scratch           | `0x08060000` | 128K  | S7     |
| Config / blackbox | `0x08080000` | 256K  | S8–S11 |

Note the F411 sector geometry (4×16K + 1×64K + 7×128K): every MCUboot slot is
deliberately aligned to one full 128K sector so that `flash_area_erase()`
never touches a neighbouring region. The APP-side backend in `../MCUBoot/`
uses the **identical** table so `boot_set_pending()` trailer addresses match
the bootloader's swap logic.

## Swap mode and image format

- Swap algorithm: **scratch-based swap** (upstream default, no `MCUBOOT_SWAP_USING_*` set)
- Confirmation: APP must call `boot_set_confirm()` after a new image runs well,
  otherwise the bootloader rolls back on next reset
- Protection: SHA-256 TLV hash only (`MCUBOOT_SIGN_*` off — chosen for flash
  budget; RSA/ECDSA can be enabled by adding the corresponding config)
- FIH hardening: off (`MCUBOOT_FIH_PROFILE_OFF`) to keep the image small
- `MCUBOOT_BOOT_MAX_ALIGN = 8`

Images are built with imgtool using:

```
imgtool sign --align 8 --confirm-only -H 0x200 -S 0x1FA00 -v <ver> ...
```

`-H 0x200` reserves space for the vector table relocation performed by the APP
(`SCB->VTOR`), `-S 0x1FA00` caps the loadable image to the 128K slot minus
trailer overhead.

## Building

This port is designed for **Keil MDK / ARM Compiler 5 (armcc)** without an
RTOS. The full bootloader Keil project additionally references:

1. Upstream MCUboot sources (`boot/util/src/loader.c`, `swap_scratch.c`,
   `swap_misc.c`, `bootutil_misc.c`, `bootutil_public.c`, `image_validate.c`,
   `tlv.c`, `fault_injection_hardening.c`) and tinycrypt
   (`ext/tinycrypt`: `sha256.c`, `utils.c`) — checked out at tag `v2.1.0`
   as a sibling directory;
2. STM32F4 standard peripheral library (ST FWLIB) and the MiniFly board
   support files (delay/sys/usart/led from the original open-source board
   project).

Those third-party trees are not redistributed here for licensing simplicity;
only this porting layer is. Include paths needed: `mcuboot/boot/bootutil/include`,
`mcuboot/boot/bootutil/src`, `mcuboot/ext/tinycrypt/lib/include`, plus the
four subdirectories of this folder.
