/*
 * MCUBoot configuration for MiniFly STM32F411CE firmware (app side).
 *
 * Must stay in sync with the bootloader's copy (Bootloader_F411 V1.1
 * \MCUBoot\mcuboot_config\mcuboot_config.h): the swap-trailer layout
 * derived from MCUBOOT_MAX_IMG_SECTORS / MCUBOOT_BOOT_MAX_ALIGN and the
 * FIH profile must match, otherwise boot_set_pending() writes trailer
 * fields at offsets the bootloader interprets differently.
 *
 * This file is included very early via fault_injection_hardening.h,
 * so compiler fix-ups must live here.
 */

#ifndef __MCUBOOT_CONFIG_H
#define __MCUBOOT_CONFIG_H

/* ---- compiler fix-ups (AC5 lacks C11 _Static_assert) ---- */
/* assert() failures are routed to the FW assertFail() via the
 * __aeabi_assert hook in mcuboot_assert.c. */
#ifdef __CC_ARM
#include <assert.h>
#define _Static_assert(e, m) typedef char mcuboot_sa_##__COUNTER__[(e) ? 1 : -1]
#endif

/* ---- FIH: turn off fault-injection hardening (keeps code small) ---- */
#define MCUBOOT_FIH_PROFILE_OFF

/* ---- crypto: software SHA-256 via bundled tinycrypt (matches bootloader) ---- */
#define MCUBOOT_USE_TINYCRYPT

/* ---- swap algorithm: scratch partition (implicit default, same as BL) ---- */

/* Use the struct-flash_sector based sector API */
#define MCUBOOT_USE_FLASH_AREA_GET_SECTORS

/* ---- image layout (MUST match bootloader) ---- */
#define MCUBOOT_MAX_IMG_SECTORS  32
#define MCUBOOT_BOOT_MAX_ALIGN   8

/* ---- watchdog: IWDG is owned by the app; feed hook is a no-op here ---- */
#define MCUBOOT_WATCHDOG_FEED()  do { } while (0)

/* ---- no logging (saves flash) ---- */
/* #undef MCUBOOT_HAVE_LOGGING */

/* ---- no signature check, hash-only (SHA256 TLV) ---- */
/* #undef MCUBOOT_SIGN_RSA */
/* #undef MCUBOOT_SIGN_EC256 */
/* #undef MCUBOOT_SIGN_ED25519 */

/* ---- no encryption ---- */
/* #undef MCUBOOT_ENC_IMAGES */

/* ---- no hardware rollback counter ---- */
/* #undef MCUBOOT_HW_ROLLBACK_PROT */

/* ---- single image ---- */
#define MCUBOOT_IMAGE_NUMBER     1

#endif /* __MCUBOOT_CONFIG_H */
