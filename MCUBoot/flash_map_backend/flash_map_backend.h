/*
 * flash_map_backend.h - MCUBoot flash area abstraction for MiniFly STM32F411CE
 * (app side; identical to the bootloader port)
 *
 * This port targets the internal flash of STM32F411CE (512KB).
 * All areas are single 128KB sectors (S5/S6/S7).
 */

#ifndef __FLASH_MAP_BACKEND_H
#define __FLASH_MAP_BACKEND_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Flash write alignment for STM32F4 word programming */
#define FLASH_AREA_ALIGN   4

struct flash_area {
    uint8_t  fa_id;
    uint8_t  fa_device_id;
    uint16_t pad16;
    uint32_t fa_off;
    uint32_t fa_size;
};

struct flash_sector {
    uint32_t fs_off;
    uint32_t fs_size;
};

static inline uint32_t flash_sector_get_off(const struct flash_sector *fs)
{
    return fs->fs_off;
}

static inline uint32_t flash_sector_get_size(const struct flash_sector *fs)
{
    return fs->fs_size;
}

/* Open/close */
int flash_area_open(uint8_t fa_id, const struct flash_area **fap);
void flash_area_close(const struct flash_area *fa);

/* I/O */
int flash_area_read(const struct flash_area *fa, uint32_t off,
                    void *dst, uint32_t len);
int flash_area_write(const struct flash_area *fa, uint32_t off,
                     const void *src, uint32_t len);
int flash_area_erase(const struct flash_area *fa, uint32_t off,
                     uint32_t len);

/* Info */
uint8_t flash_area_align(const struct flash_area *fa);
uint8_t flash_area_erased_val(const struct flash_area *fa);
uint32_t flash_area_get_size(const struct flash_area *fa);
uint32_t flash_area_get_off(const struct flash_area *fa);
uint8_t flash_area_get_id(const struct flash_area *fa);
uint8_t flash_area_get_device_id(const struct flash_area *fa);

/* Sector translation */
int flash_area_to_sectors(int fa_id, int *count, struct flash_sector *sectors);
int flash_area_get_sectors(int fa_id, uint32_t *count, struct flash_sector *sectors);

/* Slot mapping helpers */
int flash_area_id_from_multi_image_slot(int image_index, int slot);
int flash_area_id_to_multi_image_slot(int area_id);

/* Device base */
uint32_t flash_area_get_device_base(const struct flash_area *fa);

/* Utility (defined in bootutil_public.c; declared here only) */
bool bootutil_buffer_is_erased(const struct flash_area *fa,
                               const void *buf, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_MAP_BACKEND_H */
