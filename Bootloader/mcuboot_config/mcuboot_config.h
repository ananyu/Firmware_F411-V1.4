/*
 * MCUBoot configuration for MiniFly STM32F411CE (AC5 / Keil ARMCC)
 *
 * This file is included very early via fault_injection_hardening.h,
 * so compiler fix-ups must live here.
 */

#ifndef __MCUBOOT_CONFIG_H
#define __MCUBOOT_CONFIG_H

/* ---- compiler fix-ups (AC5 lacks C11 _Static_assert) ---- */
#ifdef __CC_ARM
#include <assert.h>
#define _Static_assert(e, m) typedef char mcuboot_sa_##__COUNTER__[(e) ? 1 : -1]
#endif

/* ---- FIH: turn off fault-injection hardening (keeps code small) ---- */
#define MCUBOOT_FIH_PROFILE_OFF

/* ---- crypto: software SHA-256 via bundled tinycrypt ---- */
#define MCUBOOT_USE_TINYCRYPT

/* ---- swap algorithm: scratch partition (default in bootutil_priv.h when
 *      no other mode is selected -- keep it implicit to avoid a macro
 *      redefinition warning) ---- */

/* Use the struct-flash_sector based sector API */
#define MCUBOOT_USE_FLASH_AREA_GET_SECTORS

/* ---- image layout ---- */
#define MCUBOOT_MAX_IMG_SECTORS  32
#define MCUBOOT_BOOT_MAX_ALIGN   8

/* ---- watchdog: not started in this port, feed hook is a no-op ---- */
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

/* ---- watchdog feed hook (if supported by this MCUBoot version) ---- */
/* #define MCUBOOT_WATCHDOG_FEED()  do {} while (0) */

#endif /* __MCUBOOT_CONFIG_H */
