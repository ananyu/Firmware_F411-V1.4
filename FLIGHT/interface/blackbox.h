/*
 * blackbox.h - S4 flight-log blackbox for MiniFly STM32F411.
 *
 * Partition (flash_map.h): S4 = ONE 64K sector @ 0x08010000, same flash
 * bank as the code in S5. Two hardware constraints drive the design:
 *   1. Erase granularity = the whole 64K sector, and an erase stalls every
 *      code fetch from the bank for up to ~2s -> NEVER erase while armed.
 *      Erase is reachable only through a ground command (disarmed), with
 *      the IWDG relaxed first (same trick as OTA BEGIN).
 *   2. A word program stalls fetches for ~16us -> amortised to 1 word per
 *      1kHz tick by a low-priority writer task (needs 0.6 words/tick at
 *      100Hz sampling, so there is 40% headroom; the RAM ring absorbs
 *      writer starvation, drops are counted, never block the control loop).
 *
 * Because the sector cannot be rewritten in place, this is an APPEND-ONLY
 * log, not a true ring: word 0 is a format magic, then fixed 24-byte frames
 * (6 words). The write cursor is recovered at boot by scanning for the
 * first erased slot (no header rewrite needed). When the sector fills up
 * logging stops (bbxFullVar) until a ground ERASE command clears it -
 * which also preserves the whole last flight instead of overwriting it.
 *
 * Frame layout (little-endian words):
 *   w0 seq u32 (monotonic across boots)
 *   w1 uptimeMs u32 (xTaskGetTickCount)
 *   w2 roll s16 | pitch s16   (deg x100)
 *   w3 yaw s16 (deg x100)     | vbat u16 (V x100)
 *   w4 m1 u8 | m2 u8 | m3 u8 | m4 u8   (PWM ratio 0..255)
 *   w5 fsCondBits u16 | fsLevel u8 | event u8
 * Event bits: 1 arm, 2 disarm, 4 fs->DEGRADED, 8 fs->LAND, 16 fs->KILL,
 * 32 slow-rate frame (cruise decimation; absent bit = full 100Hz rate).
 *
 * Variable rate: 100Hz for the first 10s after arm and for 5s bursts on
 * every failsafe level change; 10Hz decimated cruise in between (event
 * frames are never decimated). Capacity (65536-4)/24 = 2730 frames covers
 * ~10s full + ~173s cruise ~= 3 min of flight instead of a flat 27.3s.
 *
 * Export: DOWN_BLACKBOX(0x62) sub INFO/READ/ERASE, replies UP_BLACKBOX
 * frames (the remoter forwards unknown msgIDs transparently both ways,
 * so no remoter firmware change is needed).
 */
#ifndef __BLACKBOX_H
#define __BLACKBOX_H

#include <stdbool.h>
#include "atkp.h"

/* Boot: validate/erase-format the sector if stale, recover write cursor.
 * Call from startTask before the scheduler is busy (may erase ~2s). */
void blackboxInit(void);

/* Producer: call at 100Hz from stabilizerTask. Logs only while armed
 * (plus one final frame on disarm); pushes to the RAM ring, never blocks. */
void blackboxSample(void);

/* Writer task body: programs 1 word per 1ms tick. Static task, prio 1. */
void blackboxTask(void *param);

/* Downlink handler for msgID DOWN_BLACKBOX (runs in atkpRxAnlTask). */
void blackboxHandlePacket(const atkp_t *p);

#endif /* __BLACKBOX_H */
