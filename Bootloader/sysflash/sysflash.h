/*
 * sysflash.h - flash area ID definitions for MiniFly STM32F411CE
 *
 * Single image (image 0):
 *   slot 0 (primary)   -> S5 128K @ 0x08020000
 *   slot 1 (secondary) -> S6 128K @ 0x08040000
 *   scratch            -> S7 128K @ 0x08060000
 */

#ifndef __SYSFLASH_H
#define __SYSFLASH_H

/* Flash area IDs used by flash_map_backend.c */
#define FLASH_AREA_IMAGE_0_PRIMARY    1
#define FLASH_AREA_IMAGE_0_SECONDARY  2
#define FLASH_AREA_IMAGE_SCRATCH      3

/* MCUBoot macros for slot addressing */
#define FLASH_AREA_IMAGE_PRIMARY(x)    FLASH_AREA_IMAGE_0_PRIMARY
#define FLASH_AREA_IMAGE_SECONDARY(x)  FLASH_AREA_IMAGE_0_SECONDARY

/* Additional slot helpers referenced by loader.c (split_go path) */
int flash_area_id_from_image_slot(int slot);

#endif /* __SYSFLASH_H */
