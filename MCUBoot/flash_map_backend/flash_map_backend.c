/*
 * flash_map_backend.c - MCUBoot flash area implementation for STM32F411CE
 * (app side; identical partition table and sector handling as the
 * bootloader port so boot_set_pending() addresses match).
 *
 * Uses FWLIB (stm32f4xx_flash.c) directly. Does NOT use the old
 * STMFLASH_Write() because that auto-erases entire sectors.
 */

#include <string.h>
#include "flash_map_backend.h"
#include "sysflash.h"
#include "stm32f4xx.h"
#include "stm32f4xx_flash.h"

/* ---- partition table ---- */
static const struct flash_area flash_areas[] = {
    /* fa_id, fa_device_id, pad16, fa_off, fa_size */
    { FLASH_AREA_IMAGE_0_PRIMARY,   0, 0, 0x08020000, 128 * 1024 },
    { FLASH_AREA_IMAGE_0_SECONDARY, 0, 0, 0x08040000, 128 * 1024 },
    { FLASH_AREA_IMAGE_SCRATCH,     0, 0, 0x08060000, 128 * 1024 },
};

#define NUM_FLASH_AREAS  (sizeof(flash_areas) / sizeof(flash_areas[0]))

/* STM32F411CE sector map (512KB) */
static const struct {
    uint32_t start;
    uint32_t size;
    uint32_t sector_num;
} sector_map[] = {
    { 0x08000000,  16 * 1024, FLASH_Sector_0 },
    { 0x08004000,  16 * 1024, FLASH_Sector_1 },
    { 0x08008000,  16 * 1024, FLASH_Sector_2 },
    { 0x0800C000,  16 * 1024, FLASH_Sector_3 },
    { 0x08010000,  64 * 1024, FLASH_Sector_4 },
    { 0x08020000, 128 * 1024, FLASH_Sector_5 },
    { 0x08040000, 128 * 1024, FLASH_Sector_6 },
    { 0x08060000, 128 * 1024, FLASH_Sector_7 },
    { 0x08080000, 128 * 1024, FLASH_Sector_8 },
    { 0x080A0000, 128 * 1024, FLASH_Sector_9 },
    { 0x080C0000, 128 * 1024, FLASH_Sector_10 },
    { 0x080E0000, 128 * 1024, FLASH_Sector_11 },
};

#define NUM_SECTORS  (sizeof(sector_map) / sizeof(sector_map[0]))

static const struct flash_area *find_area(uint8_t fa_id)
{
    uint32_t i;
    for (i = 0; i < NUM_FLASH_AREAS; i++) {
        if (flash_areas[i].fa_id == fa_id) {
            return &flash_areas[i];
        }
    }
    return NULL;
}

static int get_sector_num(uint32_t addr)
{
    uint32_t i;
    for (i = 0; i < NUM_SECTORS; i++) {
        if (addr >= sector_map[i].start &&
            addr < sector_map[i].start + sector_map[i].size) {
            return sector_map[i].sector_num;
        }
    }
    return -1;
}

int flash_area_open(uint8_t fa_id, const struct flash_area **fap)
{
    const struct flash_area *fa = find_area(fa_id);
    if (fa == NULL) {
        return -1;
    }
    *fap = fa;
    return 0;
}

void flash_area_close(const struct flash_area *fa)
{
    (void)fa;
}

int flash_area_read(const struct flash_area *fa, uint32_t off,
                    void *dst, uint32_t len)
{
    uint32_t addr = fa->fa_off + off;
    memcpy(dst, (const void *)addr, len);
    return 0;
}

int flash_area_write(const struct flash_area *fa, uint32_t off,
                     const void *src, uint32_t len)
{
    uint32_t addr = fa->fa_off + off;
    const uint8_t *data = (const uint8_t *)src;
    uint32_t i;
    FLASH_Status status;

    if (len == 0) {
        return 0;
    }

    /* Address must be word-aligned for STM32F4 programming */
    if ((addr & 0x3) != 0) {
        return -1;
    }

    FLASH_Unlock();

    for (i = 0; i < len; i += 4) {
        uint32_t word;
        uint32_t chunk = (len - i) < 4 ? (len - i) : 4;

        /* Build word (pad with 0xFF if short) */
        word = 0xFFFFFFFF;
        memcpy(&word, data + i, chunk);

        status = FLASH_ProgramWord(addr + i, word);
        if (status != FLASH_COMPLETE) {
            FLASH_Lock();
            return -1;
        }
    }

    FLASH_Lock();
    return 0;
}

int flash_area_erase(const struct flash_area *fa, uint32_t off, uint32_t len)
{
    uint32_t addr = fa->fa_off + off;
    uint32_t end = addr + len;
    int sector;
    FLASH_Status status;

    if (len == 0) {
        return 0;
    }

    FLASH_Unlock();

    while (addr < end) {
        sector = get_sector_num(addr);
        if (sector < 0) {
            FLASH_Lock();
            return -1;
        }
        status = FLASH_EraseSector(sector, VoltageRange_3);
        if (status != FLASH_COMPLETE) {
            FLASH_Lock();
            return -1;
        }
        /* Move to next sector; sector_num is 0,8,16... for F411 */
        addr += sector_map[sector / 8].size;
    }

    FLASH_Lock();
    return 0;
}

uint8_t flash_area_align(const struct flash_area *fa)
{
    (void)fa;
    return FLASH_AREA_ALIGN;
}

uint8_t flash_area_erased_val(const struct flash_area *fa)
{
    (void)fa;
    return 0xFF;
}

uint32_t flash_area_get_size(const struct flash_area *fa)
{
    return fa->fa_size;
}

uint32_t flash_area_get_off(const struct flash_area *fa)
{
    return fa->fa_off;
}

uint8_t flash_area_get_id(const struct flash_area *fa)
{
    return fa->fa_id;
}

uint8_t flash_area_get_device_id(const struct flash_area *fa)
{
    return fa->fa_device_id;
}

uint32_t flash_area_get_device_base(const struct flash_area *fa)
{
    (void)fa;
    return 0x08000000;
}

int flash_area_to_sectors(int fa_id, int *count, struct flash_sector *sectors)
{
    const struct flash_area *fa = find_area((uint8_t)fa_id);
    uint32_t addr;
    uint32_t i;

    if (fa == NULL || sectors == NULL || count == NULL) {
        return -1;
    }

    if (*count < 1) {
        return -1;
    }

    addr = fa->fa_off;
    for (i = 0; i < NUM_SECTORS; i++) {
        if (addr >= sector_map[i].start &&
            addr < sector_map[i].start + sector_map[i].size) {
            sectors[0].fs_off = addr;
            sectors[0].fs_size = sector_map[i].size;
            *count = 1;
            return 0;
        }
    }

    return -1;
}

int flash_area_get_sectors(int fa_id, uint32_t *count, struct flash_sector *sectors)
{
    int c = (int)*count;
    int rc = flash_area_to_sectors(fa_id, &c, sectors);
    *count = (uint32_t)c;
    return rc;
}

int flash_area_id_from_multi_image_slot(int image_index, int slot)
{
    (void)image_index;
    switch (slot) {
    case 0: return FLASH_AREA_IMAGE_0_PRIMARY;
    case 1: return FLASH_AREA_IMAGE_0_SECONDARY;
    default: return -1;
    }
}

int flash_area_id_to_multi_image_slot(int area_id)
{
    switch (area_id) {
    case FLASH_AREA_IMAGE_0_PRIMARY:   return 0;
    case FLASH_AREA_IMAGE_0_SECONDARY: return 1;
    default: return -1;
    }
}

int flash_area_id_from_image_slot(int slot)
{
    switch (slot) {
    case 0: return FLASH_AREA_IMAGE_0_PRIMARY;
    case 1: return FLASH_AREA_IMAGE_0_SECONDARY;
    default: return -1;
    }
}
