#ifndef __KV_STORE_H
#define __KV_STORE_H

#include "sys.h"
#include <stdbool.h>

/*
 * kv_store - wear-leveled key-value store on flash sectors S2/S3.
 *
 * Design:
 *  - Two 16 KB pages (S2/S3) rotate as active/standby.
 *  - Append-only records: updating a key appends a new record, the older
 *    one is superseded by position (last write wins). No in-place rewrite,
 *    no erase on the update path.
 *  - Page full -> GC copies live records to the standby page and commits
 *    it by writing the page-header magic (last step = commit point).
 *  - Power loss is safe at every step: worst case loses the in-flight
 *    record, never previously committed data. Torn records are detected
 *    by CRC at the next boot scan and cleaned by GC.
 *  - RAM index maps key -> latest record address, so kv_get() is O(n)
 *    over a tiny table and touches flash only for the value itself.
 *
 * Key namespace (u16):
 *   0x0000-0x00FF  flight parameters (PID, trim, ...)
 *   0x0100-0x01FF  calibration data
 *   0x0200-0x02FF  blackbox / diagnostics
 *   0xFFFF         invalid
 */

#define KV_NS_PARAM       0x0000U
#define KV_NS_CALIB       0x0100U
#define KV_NS_BLACKBOX    0x0200U
#define KV_KEY_INVALID    0xFFFFU

#define KV_VALUE_MAX_LEN  64U   /* max value size in bytes          */
#define KV_MAX_KEYS       32U   /* max distinct keys in RAM index   */

/* Mount the store. If neither page is valid (fresh chip or stale bytes
 * left in S2/S3 by older app builds), both pages are formatted.
 * Safe to call before the scheduler runs. */
bool kv_init(void);

/* Erase both pages, re-initialize page A with seq=1, drop all keys. */
bool kv_format(void);

/* Read the latest value of key.
 * *len: in = buffer capacity, out = actual value length.
 * Returns false if key not found, or buffer too small (*len = needed). */
bool kv_get(u16 key, void *buf, u16 *len);

/* Store a value for key (append a new record). Writing the identical
 * value again is a no-op. Triggers GC when the active page has no room.
 * len must be 1..KV_VALUE_MAX_LEN. */
bool kv_set(u16 key, const void *data, u16 len);

/* Bytes used in the active page (header + records). Debug/test aid. */
u32  kv_active_used(void);

#endif /* __KV_STORE_H */
