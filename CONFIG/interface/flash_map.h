#ifndef __FLASH_MAP_H
#define __FLASH_MAP_H

#include "stmflash.h"   /* STM32_FLASH_BASE, ADDR_FLASH_SECTOR_x */

/*
 * MiniFly STM32F411CE (512KB) flash partition map -- single source of truth.
 *
 * Physical sectors:
 *   S0  16K 0x08000000   S1  16K 0x08004000
 *   S2  16K 0x08008000   S3  16K 0x0800C000
 *   S4  64K 0x08010000
 *   S5 128K 0x08020000   S6 128K 0x08040000   S7 128K 0x08060000
 *
 * Layout (active NOW, no transitional layout):
 *   S0     16K  bootloader
 *                 - now: stock Bootloader_F411, jump target moved to FM_APP_LINK_BASE
 *                 - later: MCUBoot (grows into S1, 32K total; flashing MCUBoot
 *                   erases S1 -- legacy params are abandoned, accepted)
 *   S1     16K  reserved / abandoned (legacy config_param area; replaced
 *                 by the KV store in S2/S3, no data migration -- first
 *                 boot with KV firmware starts from built-in defaults)
 *   S2     16K  KV store page A
 *   S3     16K  KV store page B  (dual-page rotation: wear leveling + atomic commit)
 *   S4     64K  blackbox (flight log ring buffer)
 *   S5    128K  slot 0 (running app, linked at FM_APP_LINK_BASE)
 *   S6    128K  slot 1 (OTA staging; unused until MCUBoot)
 *   S7    128K  scratch (MCUBoot swap area; unused until MCUBoot)
 *
 * Why identical 128K single-sector slots: MCUBoot swap-scratch walks the
 * sector tables of slot0/slot1 in parallel and needs scratch >= largest
 * sector. {128K}/{128K}/scratch=128K is the only unambiguous layout that
 * also leaves room for KV + blackbox on a 512K part.
 * Cost: app is capped at FM_APP_MAX_SIZE (~126.5KB); stock app is ~101.6KB.
 *
 * Note: KV/blackbox init must treat pages holding stale bytes (old app code
 * in S2..S4 from the pre-relocation build) as "invalid -> erase + format".
 */

/* ---------------- bootloader ---------------- */
#define FM_BOOT_BASE            ADDR_FLASH_SECTOR_0      /* 0x08000000 */
#define FM_BOOT_SIZE            (32U*1024U)              /* S0+S1 reserved for MCUBoot */

/* ---------------- KV store (dual page) ---------------- */
#define FM_KV_A_BASE            ADDR_FLASH_SECTOR_2      /* 0x08008000 */
#define FM_KV_B_BASE            ADDR_FLASH_SECTOR_3      /* 0x0800C000 */
#define FM_KV_PAGE_SIZE         (16U*1024U)

/* ---------------- blackbox (flight log) ---------------- */
#define FM_BLACKBOX_BASE        ADDR_FLASH_SECTOR_4      /* 0x08010000 */
#define FM_BLACKBOX_SIZE        (64U*1024U)

/* ---------------- app slots (MCUBoot) ---------------- */
#define FM_SLOT0_BASE           ADDR_FLASH_SECTOR_5      /* 0x08020000 */
#define FM_SLOT0_SIZE           (128U*1024U)

#define FM_SLOT1_BASE           ADDR_FLASH_SECTOR_6      /* 0x08040000 */
#define FM_SLOT1_SIZE           (128U*1024U)

#define FM_SCRATCH_BASE         ADDR_FLASH_SECTOR_7      /* 0x08060000 */
#define FM_SCRATCH_SIZE         (128U*1024U)

/* MCUBoot image overhead: header + swap trailer reserve (recompute at port) */
#define FM_IMG_HDR_SIZE         0x200U                   /* 512B image header */
#define FM_IMG_TRAILER_RSVD     0x400U                   /* swap status/magic reserve */
#define FM_APP_MAX_SIZE         (FM_SLOT0_SIZE - FM_IMG_HDR_SIZE - FM_IMG_TRAILER_RSVD)

/* app link base -- MUST match FIRMWARE_START_ADDR in app config.h (VTOR source)
 * and the jump target in the bootloader                                    */
#define FM_APP_LINK_BASE        (FM_SLOT0_BASE + FM_IMG_HDR_SIZE)   /* 0x08020200 */

/* ---------------- sanity checks ---------------- */
typedef char fm_check_total[(FM_SCRATCH_BASE + FM_SCRATCH_SIZE == STM32_FLASH_BASE + 512U*1024U) ? 1 : -1];
typedef char fm_check_slots[(FM_SLOT0_SIZE == FM_SLOT1_SIZE) ? 1 : -1];
typedef char fm_check_scratch[(FM_SCRATCH_SIZE >= FM_SLOT1_SIZE) ? 1 : -1];
typedef char fm_check_link[(FM_APP_LINK_BASE == 0x08020200UL) ? 1 : -1];

#endif /* __FLASH_MAP_H */
