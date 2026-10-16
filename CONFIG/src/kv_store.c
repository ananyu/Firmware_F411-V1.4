#include "kv_store.h"
#include "flash_map.h"

#include "stm32f4xx_flash.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* On-flash layout                                                     */
/* ------------------------------------------------------------------ */

#define KV_PAGE_MAGIC     0x4B563153UL   /* "KV1S"                        */
#define KV_PAGE_HDR_SIZE  16U            /* magic(4) + seq(4) + rsvd(8)   */
#define KV_REC_HDR_SIZE   8U             /* key(2)+len(2)+crc(2)+rsvd(2)  */
#define KV_REC_HDR_RSVD   0xFFFFU        /* kept erased: future flags     */

#define KV_ALIGN4(x)      (((x) + 3U) & ~3U)

/* Page header, 16 bytes. During GC it is written in two steps:
 * seq first, magic last -> magic acts as the commit flag. */
typedef struct {
    u32 magic;
    u32 seq;
    u32 rsvd[2];
} kv_page_hdr_t;

/* Record header, 8 bytes, followed by data padded to 4-byte words.
 * Pad bytes carry 0xFF (the unprogrammed value). crc16 covers
 * key + len + data (not rsvd, not pad). */
typedef struct {
    u16 key;
    u16 len;
    u16 crc;
    u16 rsvd;
} kv_rec_hdr_t;

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    u16 key;
    u32 addr;   /* absolute address of the latest record header */
} kv_slot_t;

static kv_slot_t  s_idx[KV_MAX_KEYS];
static u8         s_nkeys;
static u32        s_active;    /* base address of active page      */
static u32        s_standby;   /* base address of standby page     */
static u32        s_seq;       /* sequence number of active page   */
static u32        s_tail;      /* next free address in active page */
static bool       s_mounted;
static SemaphoreHandle_t s_lock;

/* ------------------------------------------------------------------ */
/* Locking (safe before and after the scheduler runs)                  */
/* ------------------------------------------------------------------ */

static bool kv_lock(void)
{
    if (s_lock != NULL && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        return true;
    }
    return false;
}

static void kv_unlock(bool locked)
{
    if (locked) {
        xSemaphoreGive(s_lock);
    }
}

/* ------------------------------------------------------------------ */
/* CRC-16/CCITT (poly 0x1021, init 0xFFFF)                             */
/* ------------------------------------------------------------------ */

static u16 kv_crc16_update(u16 crc, const void *data, u16 len)
{
    const u8 *p = (const u8 *)data;
    u8 i;

    while (len--) {
        crc ^= (u16)(*p++) << 8;
        for (i = 0; i < 8U; i++) {
            crc = (crc & 0x8000U) ? (u16)((crc << 1) ^ 0x1021U) : (u16)(crc << 1);
        }
    }
    return crc;
}

/* ------------------------------------------------------------------ */
/* Flash primitives                                                    */
/*                                                                     */
/* Deliberately NOT using STMFLASH_Write(): it auto-erases the whole   */
/* sector when it finds a non-erased word (stmflash.c), which would    */
/* destroy every record on the page at the second append. KV pages are */
/* erased only by kv_erase_page() during format/GC; appends always     */
/* program pre-erased words.                                           */
/* ------------------------------------------------------------------ */

static void kv_read(u32 addr, void *buf, u32 len)
{
    memcpy(buf, (const void *)addr, len);
}

/* Program len bytes (must be a multiple of 4) into erased space. */
static bool kv_write(u32 addr, const void *data, u32 len)
{
    const u8 *p = (const u8 *)data;
    bool ok = true;
    u32 i;
    u32 w;

    FLASH_Unlock();
    FLASH_DataCacheCmd(DISABLE);
    for (i = 0; i < len && ok; i += 4U) {
        memcpy(&w, p + i, sizeof(w));
        ok = (FLASH_ProgramWord(addr + i, w) == FLASH_COMPLETE);
    }
    FLASH_DataCacheCmd(ENABLE);
    FLASH_Lock();
    return ok;
}

static bool kv_erase_page(u32 base)
{
    u32 sector = (base == FM_KV_A_BASE) ? FLASH_Sector_2 : FLASH_Sector_3;
    FLASH_Status st;

    FLASH_Unlock();
    FLASH_DataCacheCmd(DISABLE);
    st = FLASH_EraseSector(sector, VoltageRange_3);
    FLASH_DataCacheCmd(ENABLE);
    FLASH_Lock();
    return (st == FLASH_COMPLETE);
}

/* ------------------------------------------------------------------ */
/* Page header helpers                                                 */
/* ------------------------------------------------------------------ */

static void kv_read_page_hdr(u32 base, kv_page_hdr_t *h)
{
    kv_read(base, h, sizeof(*h));
}

static bool kv_page_valid(const kv_page_hdr_t *h)
{
    return (h->magic == KV_PAGE_MAGIC);
}

static bool kv_write_page_seq(u32 base, u32 seq)
{
    return kv_write(base + 4U, &seq, sizeof(seq));
}

static bool kv_write_page_magic(u32 base)
{
    u32 magic = KV_PAGE_MAGIC;
    return kv_write(base, &magic, sizeof(magic));
}

/* ------------------------------------------------------------------ */
/* RAM index                                                           */
/* ------------------------------------------------------------------ */

static kv_slot_t *kv_idx_find(u16 key)
{
    u8 i;

    for (i = 0; i < s_nkeys; i++) {
        if (s_idx[i].key == key) {
            return &s_idx[i];
        }
    }
    return NULL;
}

static void kv_idx_put(u16 key, u32 addr)
{
    kv_slot_t *s = kv_idx_find(key);

    if (s != NULL) {
        s->addr = addr;
    } else if (s_nkeys < KV_MAX_KEYS) {
        s_idx[s_nkeys].key  = key;
        s_idx[s_nkeys].addr = addr;
        s_nkeys++;
    }
}

/* ------------------------------------------------------------------ */
/* Scan the active page and rebuild the RAM index.                     */
/* Returns true if a corrupt/torn record was found (caller should GC). */
/* The tail pointer ends on the first erased or invalid header.        */
/* ------------------------------------------------------------------ */

static bool kv_scan(void)
{
    bool need_gc = false;
    u32 off = s_active + KV_PAGE_HDR_SIZE;
    const u32 end = s_active + FM_KV_PAGE_SIZE;
    kv_rec_hdr_t h;
    u16 crc;
    u32 rec_len;
    u8 val[KV_VALUE_MAX_LEN];

    s_nkeys = 0;

    while (off + KV_REC_HDR_SIZE <= end) {
        kv_read(off, &h, sizeof(h));

        if (h.key == 0xFFFFU && h.len == 0xFFFFU) {
            break;  /* erased header = end of records */
        }
        if (h.key == KV_KEY_INVALID || h.len == 0U || h.len > KV_VALUE_MAX_LEN) {
            need_gc = true;
            break;
        }
        rec_len = KV_REC_HDR_SIZE + KV_ALIGN4(h.len);
        if (off + rec_len > end) {
            need_gc = true;
            break;
        }
        crc = 0xFFFFU;
        crc = kv_crc16_update(crc, &h.key, sizeof(h.key));
        crc = kv_crc16_update(crc, &h.len, sizeof(h.len));
        kv_read(off + KV_REC_HDR_SIZE, val, h.len);
        crc = kv_crc16_update(crc, val, h.len);
        if (crc != h.crc) {
            need_gc = true;  /* torn write (power loss) or corruption */
            break;
        }
        kv_idx_put(h.key, off);
        off += rec_len;
    }

    s_tail = off;
    return need_gc;
}

/* ------------------------------------------------------------------ */
/* Garbage collection                                                  */
/*                                                                     */
/* 1. erase standby (also cleans a half-finished previous GC)          */
/* 2. write seq; magic stays erased -> page not valid yet              */
/* 3. copy live records                                                */
/* 4. write magic -> commit point                                      */
/* 5. swap roles; the old page keeps its stale-but-valid header and    */
/*    is erased at step 1 of the next GC. Boot resolves two valid      */
/*    pages by picking the higher seq.                                 */
/* Power loss before step 4: new page invalid, old page intact.        */
/* ------------------------------------------------------------------ */

static bool kv_gc(void)
{
    u32 dst;
    u32 new_seq = s_seq + 1U;
    u8 i;
    kv_rec_hdr_t h;
    u32 rec_len;
    u32 old;

    if (!kv_erase_page(s_standby)) {
        return false;
    }
    if (!kv_write_page_seq(s_standby, new_seq)) {
        return false;
    }

    dst = s_standby + KV_PAGE_HDR_SIZE;
    for (i = 0; i < s_nkeys; i++) {
        kv_read(s_idx[i].addr, &h, sizeof(h));
        rec_len = KV_REC_HDR_SIZE + KV_ALIGN4(h.len);
        if (!kv_write(dst, (const void *)s_idx[i].addr, rec_len)) {
            return false;  /* new page has seq but no magic -> invalid */
        }
        s_idx[i].addr = dst;
        dst += rec_len;
    }

    if (!kv_write_page_magic(s_standby)) {
        return false;
    }

    old       = s_active;
    s_active  = s_standby;
    s_standby = old;
    s_seq     = new_seq;
    s_tail    = dst;
    return true;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

bool kv_format(void)
{
    bool locked = kv_lock();

    if (!kv_erase_page(FM_KV_A_BASE) || !kv_erase_page(FM_KV_B_BASE)) {
        kv_unlock(locked);
        return false;
    }
    if (!kv_write_page_seq(FM_KV_A_BASE, 1U) || !kv_write_page_magic(FM_KV_A_BASE)) {
        kv_unlock(locked);
        return false;
    }
    s_active  = FM_KV_A_BASE;
    s_standby = FM_KV_B_BASE;
    s_seq     = 1U;
    s_tail    = FM_KV_A_BASE + KV_PAGE_HDR_SIZE;
    s_nkeys   = 0;

    kv_unlock(locked);
    return true;
}

bool kv_init(void)
{
    kv_page_hdr_t a;
    kv_page_hdr_t b;
    bool av;
    bool bv;

    if (s_mounted) {
        return true;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }

    kv_read_page_hdr(FM_KV_A_BASE, &a);
    kv_read_page_hdr(FM_KV_B_BASE, &b);
    av = kv_page_valid(&a);
    bv = kv_page_valid(&b);

    if (!av && !bv) {
        /* No valid page: fresh chip, or stale bytes left by older builds. */
        if (!kv_format()) {
            return false;
        }
    } else {
        if (av && (!bv || a.seq >= b.seq)) {
            s_active  = FM_KV_A_BASE;
            s_standby = FM_KV_B_BASE;
            s_seq     = a.seq;
        } else {
            s_active  = FM_KV_B_BASE;
            s_standby = FM_KV_A_BASE;
            s_seq     = b.seq;
        }
        if (kv_scan() && !kv_gc()) {
            return false;  /* corrupt tail could not be cleaned */
        }
    }

    s_mounted = true;
    return true;
}

bool kv_get(u16 key, void *buf, u16 *len)
{
    kv_slot_t *s;
    kv_rec_hdr_t h;
    bool locked;

    if (!s_mounted || buf == NULL || len == NULL) {
        return false;
    }
    locked = kv_lock();
    s = kv_idx_find(key);
    if (s == NULL) {
        kv_unlock(locked);
        return false;
    }
    kv_read(s->addr, &h, sizeof(h));
    if (*len < h.len) {
        *len = h.len;
        kv_unlock(locked);
        return false;
    }
    kv_read(s->addr + KV_REC_HDR_SIZE, buf, h.len);
    *len = h.len;
    kv_unlock(locked);
    return true;
}

bool kv_set(u16 key, const void *data, u16 len)
{
    kv_rec_hdr_t h;
    u16 crc;
    u32 need;
    bool locked;
    kv_slot_t *s;
    kv_rec_hdr_t ch;
    u8 cur[KV_VALUE_MAX_LEN];
    u8 payload[KV_VALUE_MAX_LEN];
    u32 padded;

    if (!s_mounted || data == NULL || len == 0U || len > KV_VALUE_MAX_LEN ||
        key == KV_KEY_INVALID) {
        return false;
    }

    locked = kv_lock();

    s = kv_idx_find(key);
    if (s == NULL && s_nkeys >= KV_MAX_KEYS) {
        kv_unlock(locked);
        return false;  /* index full: new key rejected */
    }

    /* dedup: identical value already stored -> nothing to do */
    if (s != NULL) {
        kv_read(s->addr, &ch, sizeof(ch));
        if (ch.len == len) {
            kv_read(s->addr + KV_REC_HDR_SIZE, cur, len);
            if (memcmp(cur, data, len) == 0) {
                kv_unlock(locked);
                return true;
            }
        }
    }

    need = KV_REC_HDR_SIZE + KV_ALIGN4(len);
    if (s_tail + need > s_active + FM_KV_PAGE_SIZE) {
        if (!kv_gc()) {
            kv_unlock(locked);
            return false;
        }
        if (s_tail + need > s_active + FM_KV_PAGE_SIZE) {
            kv_unlock(locked);
            return false;  /* cannot happen with len<=64, stay defensive */
        }
    }

    /* Header first, data after. A power loss between the two leaves a
     * record with a bad CRC: the next boot scan truncates there and GC
     * cleans the residue; the previous value stays valid meanwhile. */
    crc = 0xFFFFU;
    crc = kv_crc16_update(crc, &key, sizeof(key));
    crc = kv_crc16_update(crc, &len, sizeof(len));
    crc = kv_crc16_update(crc, data, len);

    h.key  = key;
    h.len  = len;
    h.crc  = crc;
    h.rsvd = KV_REC_HDR_RSVD;

    if (!kv_write(s_tail, &h, KV_REC_HDR_SIZE)) {
        kv_unlock(locked);
        return false;
    }

    padded = KV_ALIGN4(len);
    memset(payload, 0xFF, padded);   /* pad bytes stay "unprogrammed" */
    memcpy(payload, data, len);
    if (!kv_write(s_tail + KV_REC_HDR_SIZE, payload, padded)) {
        kv_unlock(locked);
        return false;
    }

    kv_idx_put(key, s_tail);
    s_tail += need;

    kv_unlock(locked);
    return true;
}

u32 kv_active_used(void)
{
    return s_mounted ? (s_tail - s_active) : 0U;
}
