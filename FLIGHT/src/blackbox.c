/*
 * blackbox.c - S4 append-only flight-log blackbox. See blackbox.h for the
 * partition constraints and frame layout.
 *
 * Data flow:
 *   stabilizerTask (100Hz, armed only) --push--> RAM ring (128 frames)
 *   blackboxTask (prio 1, 1ms tick)     --pop---> 1 flash word program
 *   atkpRxAnlTask (ground commands)     --READ/ERASE--> uplink replies
 *
 * The writer never erases (boot/ground only) and programs a single word per
 * tick, so the worst-case fetch stall (~16us) lands inside a 1ms idle budget
 * and cannot disturb the 1kHz control loop.
 *
 * All storage static - zero dynamic allocation (project hard constraint).
 */
#include "blackbox.h"
#include "flash_map.h"
#include "atkp.h"
#include "commander.h"
#include "failsafe.h"
#include "power_control.h"
#include "stabilizer.h"
#include "pm.h"
#include "radiolink.h"
#include "watchdog.h"
#include "debug_assert.h"
#include "stm32f4xx_flash.h"
#include "stm32f4xx_iwdg.h"
#include "FreeRTOS.h"
#include "task.h"
#include "freertos_static.h"
#include <string.h>

/* ---- frame geometry ---- */
#define BBX_MAGIC        0x42425831u	/* "BBX1" */
#define BBX_HDR_WORDS    1u		/* magic word at sector offset 0 */
#define BBX_FRAME_WORDS  6u		/* 24 bytes per frame */
#define BBX_FRAME_BYTES  (BBX_FRAME_WORDS * 4u)
#define BBX_MAX_FRAMES   ((FM_BLACKBOX_SIZE - BBX_HDR_WORDS * 4u) / BBX_FRAME_BYTES) /* 2730 */

/* event bits (frame w5) */
#define BBX_EV_ARM       0x01u
#define BBX_EV_DISARM    0x02u
#define BBX_EV_DEGRADED  0x04u
#define BBX_EV_LAND      0x08u
#define BBX_EV_KILL      0x10u

/* ---- RAM ring ---- */
#define BBX_RING_LEN     128u		/* power of two; 128 x 24B = 3072B RAM */
static uint32_t s_ring[BBX_RING_LEN * BBX_FRAME_WORDS];
static volatile uint16_t s_wr, s_rd;	/* free-running indices & (LEN-1) */
static volatile uint32_t s_dropped;

/* ---- flash cursor (words into FM_BLACKBOX_BASE) ---- */
static uint32_t s_writeWord;		/* next word to program */
static uint32_t s_seq;			/* monotonic frame sequence (saved in frames) */

/* SWD-inspectable state (non-static on purpose, like failsafe vars). */
uint32_t bbxWriteWordVar;			/* words consumed of 16384 */
uint32_t bbxSeqVar;
uint32_t bbxDroppedVar;
uint8_t  bbxFullVar;				/* 1 = sector exhausted, logging stopped */
uint8_t  bbxBootedVar;				/* 1 = blackboxInit() completed */

/* ---- protocol ---- */
#define BBX_SUB_INFO   0x01u
#define BBX_SUB_READ   0x02u
#define BBX_SUB_ERASE  0x03u
#define UP_BBX_INFO    0x01u
#define UP_BBX_DATA    0x02u
#define UP_BBX_ACK     0x03u

/* ======================= flash primitives ======================= */

/* Bounded word program, interrupt-masked, read-back verified - same
 * hardening as otaProgramWordAttempt() (a stuck BSY must not wedge the
 * writer forever; 16us spec vs 50000-loop timeout). */
static bool bbxProgramWord(uint32_t addr, uint32_t word)
{
	uint32_t primask, timeout;
	bool ok;

	primask = __get_PRIMASK();
	__disable_irq();

	FLASH_Unlock();
	FLASH_ClearFlag(FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
			FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
	FLASH->CR &= ~((uint32_t)0x3 << 8);	/* PSIZE mask */
	FLASH->CR |= FLASH_PSIZE_WORD;
	FLASH->CR |= FLASH_CR_PG;
	*(volatile uint32_t *)addr = word;

	timeout = 50000u;
	while (((FLASH->SR & FLASH_FLAG_BSY) != 0) && (timeout-- != 0)) {
	}
	ok = ((FLASH->SR & FLASH_FLAG_BSY) == 0) &&
	     ((FLASH->SR & (FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
			     FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR)) == 0) &&
	     (*(volatile uint32_t *)addr == word);

	FLASH->CR &= ~FLASH_CR_PG;
	FLASH_Lock();
	if (primask == 0) {
		__enable_irq();
	}
	return ok;
}

/* Erase the whole 64K sector with the IWDG relaxed (erase stalls ~2s max;
 * the stock 150ms dog would reset mid-erase). Ground/disarmed only. */
static bool bbxEraseSector(void)
{
	uint32_t primask, timeout;
	bool ok;

	IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
	IWDG_SetPrescaler(IWDG_Prescaler_256);
	IWDG_SetReload(0xFFF);
	IWDG_WriteAccessCmd(IWDG_WriteAccess_Disable);
	IWDG_ReloadCounter();

	primask = __get_PRIMASK();
	__disable_irq();

	FLASH_Unlock();
	FLASH_ClearFlag(FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
			FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
	FLASH_EraseSector(FLASH_Sector_4, VoltageRange_3);
	/* ~14s budget: spec max sector erase is 2s; loop body ~10 cycles
	 * at 96MHz -> 134M iterations ~ 14s worst case (IWDG relaxed to 32s) */
	timeout = 0x08000000u;
	while (((FLASH->SR & FLASH_FLAG_BSY) != 0) && (timeout-- != 0)) {
	}
	ok = ((FLASH->SR & FLASH_FLAG_BSY) == 0) &&
	     ((*(volatile uint32_t *)FM_BLACKBOX_BASE) == 0xFFFFFFFFu);

	FLASH_Lock();
	if (primask == 0) {
		__enable_irq();
	}

	watchdogReset();
	return ok;
}

/* ======================= boot scan ======================= */

/* Append-only recovery. Scan word-by-word (not frame-by-frame) for the
 * first erased word: a power loss mid-frame leaves words 0..k programmed
 * and k+1..5 erased, so a frame-granular scan would skip the hole and
 * write into programmed flash. Round the hole up to the next frame
 * boundary (abandon the partial frame, at most 5 words per boot) to keep
 * the frame-aligned invariant that READ relies on. seq = last complete
 * frame's seq, so numbering continues across reboots. */
static void bbxRecoverCursor(void)
{
	uint32_t w;
	const uint32_t end = FM_BLACKBOX_SIZE / 4u;

	if (*(volatile uint32_t *)FM_BLACKBOX_BASE != BBX_MAGIC) {
		/* stale bytes (pre-blackbox firmware) or blank sector: format */
		if (!bbxEraseSector()) {
			return;	/* leave bbxBootedVar=0, logging disabled */
		}
		if (!bbxProgramWord(FM_BLACKBOX_BASE, BBX_MAGIC)) {
			return;
		}
		s_writeWord = BBX_HDR_WORDS;
		s_seq = 0u;
	} else {
		for (w = BBX_HDR_WORDS; w < end; w++) {
			if (*(volatile uint32_t *)(FM_BLACKBOX_BASE + w * 4u) == 0xFFFFFFFFu) {
				break;
			}
		}
		if (w >= end) {
			s_writeWord = end;		/* sector full */
			bbxFullVar = 1;
		} else {
			uint32_t off = w - BBX_HDR_WORDS;	/* round up to frame boundary */
			s_writeWord = ((off + BBX_FRAME_WORDS - 1u) / BBX_FRAME_WORDS) *
				BBX_FRAME_WORDS + BBX_HDR_WORDS;
		}
		if (s_writeWord >= BBX_HDR_WORDS + BBX_FRAME_WORDS) {
			s_seq = *(volatile uint32_t *)(FM_BLACKBOX_BASE +
				(s_writeWord - BBX_FRAME_WORDS) * 4u);
		} else {
			s_seq = 0u;
		}
	}
	bbxWriteWordVar = s_writeWord;
	bbxSeqVar = s_seq;
	bbxBootedVar = 1;
}

void blackboxInit(void)
{
	bbxRecoverCursor();
}

/* ======================= producer ======================= */

static uint8_t s_prevLevel;
static bool s_prevArmed;
static bool s_finalPending;

static void bbxPush(const uint32_t *frame)
{
	uint16_t next = (uint16_t)((s_wr + 1u) & (BBX_RING_LEN - 1u));
	if (next == s_rd) {
		s_dropped++;		/* writer starved; never block the control loop */
		return;
	}
	memcpy(&s_ring[s_wr * BBX_FRAME_WORDS], frame, BBX_FRAME_BYTES);
	s_wr = next;
}

void blackboxSample(void)
{
	attitude_t att;
	motorPWM_t mp;
	uint32_t f[BBX_FRAME_WORDS];
	uint8_t ev = 0u;
	uint8_t lvl;
	bool armed = atkpIsFlightUnlocked();

	lvl = failsafeLevel();
	if (armed && !s_prevArmed) {
		ev |= BBX_EV_ARM;
	}
	if (!armed && s_prevArmed) {
		ev |= BBX_EV_DISARM;
		s_finalPending = true;	/* one frame after landing/disarm */
	}
	if (lvl != s_prevLevel) {
		if (lvl == FS_DEGRADED) ev |= BBX_EV_DEGRADED;
		else if (lvl == FS_LAND) ev |= BBX_EV_LAND;
		else if (lvl == FS_KILL) ev |= BBX_EV_KILL;
		s_prevLevel = lvl;
	}
	s_prevArmed = armed;

	if (!armed && !s_finalPending) {
		return;		/* ground idle: no logging, no flash wear */
	}

	getAttitudeData(&att);
	getMotorPWM(&mp);

	f[0] = ++s_seq;
	f[1] = (uint32_t)xTaskGetTickCount();
	f[2] = ((uint32_t)(uint16_t)(int16_t)(att.roll * 100.0f)) |
	       (((uint32_t)(uint16_t)(int16_t)(att.pitch * 100.0f)) << 16);
	f[3] = ((uint32_t)(uint16_t)(int16_t)(att.yaw * 100.0f)) |
	       (((uint32_t)(uint16_t)(int16_t)(pmGetBatteryVoltage() * 100.0f)) << 16);
	f[4] = ((uint32_t)(mp.m1 > 255u ? 255u : mp.m1)) |
	       ((uint32_t)(mp.m2 > 255u ? 255u : mp.m2) << 8) |
	       ((uint32_t)(mp.m3 > 255u ? 255u : mp.m3) << 16) |
	       ((uint32_t)(mp.m4 > 255u ? 255u : mp.m4) << 24);
	f[5] = (uint32_t)failsafeConditionBits() | ((uint32_t)lvl << 16) |
	       ((uint32_t)ev << 24);

	bbxPush(f);

	if (s_finalPending && !armed) {
		s_finalPending = false;	/* the one final frame is queued */
	}
}

/* ======================= writer task ======================= */

/* Word-level amortisation: pop one frame from the RAM ring into a staging
 * buffer, then program ONE word per 1ms tick (~16us fetch stall, invisible
 * to the 1kHz loop). 100Hz production = 600 words/s < 1000 words/s capacity. */
static uint32_t s_stage[BBX_FRAME_WORDS];
static uint8_t  s_stageIdx = 0xFFu;	/* 0xFF = no frame in flight */

void blackboxTask(void *param)
{
	(void)param;

	for (;;) {
		vTaskDelay(1);
		if (!bbxBootedVar || bbxFullVar) {
			continue;
		}

		if (s_stageIdx == 0xFFu) {
			if (s_rd == s_wr) {
				continue;
			}
			if (s_writeWord + BBX_FRAME_WORDS > FM_BLACKBOX_SIZE / 4u) {
				bbxFullVar = 1;	/* sector exhausted: stop, keep data */
				continue;
			}
			memcpy(s_stage, &s_ring[s_rd * BBX_FRAME_WORDS], BBX_FRAME_BYTES);
			s_stageIdx = 0u;
		}

		if (!bbxProgramWord(FM_BLACKBOX_BASE +
				(s_writeWord + s_stageIdx) * 4u, s_stage[s_stageIdx])) {
			bbxFullVar = 1;		/* program fault: stop logging */
			bbxDroppedVar = s_dropped;
			continue;
		}
		s_stageIdx++;
		if (s_stageIdx >= BBX_FRAME_WORDS) {
			s_stageIdx = 0xFFu;
			s_writeWord += BBX_FRAME_WORDS;
			bbxWriteWordVar = s_writeWord;
			bbxSeqVar = s_seq;
			s_rd = (uint16_t)((s_rd + 1u) & (BBX_RING_LEN - 1u));
		}
		bbxDroppedVar = s_dropped;
	}
}

/* ======================= downlink protocol ======================= */

static void bbxSend(const uint8_t *data, uint8_t len)
{
	atkp_t p;
	uint8_t i;

	p.msgID = UP_BLACKBOX;
	p.dataLen = len;
	for (i = 0u; i < len; i++) {
		p.data[i] = data[i];
	}
	radiolinkSendPacketBlocking(&p);
}

static void bbxSendInfo(void)
{
	uint8_t d[12];
	uint32_t frames = (s_writeWord - BBX_HDR_WORDS) / BBX_FRAME_WORDS;

	d[0] = UP_BBX_INFO;
	d[1] = (uint8_t)(frames);        d[2] = (uint8_t)(frames >> 8);
	d[3] = (uint8_t)(frames >> 16);  d[4] = (uint8_t)(frames >> 24);
	d[5] = (uint8_t)(s_seq);         d[6] = (uint8_t)(s_seq >> 8);
	d[7] = (uint8_t)(s_seq >> 16);   d[8] = (uint8_t)(s_seq >> 24);
	d[9] = bbxFullVar;
	d[10] = (uint8_t)(s_dropped);
	d[11] = bbxBootedVar;
	bbxSend(d, sizeof(d));
}

void blackboxHandlePacket(const atkp_t *p)
{
	uint8_t sub;
	uint32_t idx, w, i;
	uint8_t d[3 + BBX_FRAME_BYTES];

	if (p->dataLen < 1u) {
		return;
	}
	sub = p->data[0];

	if (sub == BBX_SUB_INFO) {
		bbxSendInfo();
	} else if (sub == BBX_SUB_READ && p->dataLen >= 3u) {
		idx = (uint32_t)p->data[1] | ((uint32_t)p->data[2] << 8);
		if (!bbxBootedVar || idx >= BBX_MAX_FRAMES) {
			d[0] = UP_BBX_ACK; d[1] = 0u;	/* invalid index */
			bbxSend(d, 2u);
			return;
		}
		w = BBX_HDR_WORDS + idx * BBX_FRAME_WORDS;
		if (w + BBX_FRAME_WORDS > s_writeWord) {
			d[0] = UP_BBX_ACK; d[1] = 0u;	/* beyond written end */
			bbxSend(d, 2u);
			return;
		}
		d[0] = UP_BBX_DATA;
		d[1] = (uint8_t)idx; d[2] = (uint8_t)(idx >> 8);
		for (i = 0u; i < BBX_FRAME_WORDS; i++) {
			uint32_t v = *(volatile uint32_t *)(FM_BLACKBOX_BASE + w * 4u + i * 4u);
			d[3 + i * 4u] = (uint8_t)v;
			d[4 + i * 4u] = (uint8_t)(v >> 8);
			d[5 + i * 4u] = (uint8_t)(v >> 16);
			d[6 + i * 4u] = (uint8_t)(v >> 24);
		}
		bbxSend(d, 3u + BBX_FRAME_BYTES);
	} else if (sub == BBX_SUB_ERASE) {
		if (atkpIsFlightUnlocked()) {
			d[0] = UP_BBX_ACK; d[1] = 0u;	/* refuse while armed */
			bbxSend(d, 2u);
			return;
		}
		if (bbxEraseSector() && bbxProgramWord(FM_BLACKBOX_BASE, BBX_MAGIC)) {
			s_writeWord = BBX_HDR_WORDS;
			bbxWriteWordVar = s_writeWord;
			bbxFullVar = 0u;
			s_dropped = 0u;
			d[0] = UP_BBX_ACK; d[1] = 1u;
		} else {
			d[0] = UP_BBX_ACK; d[1] = 0u;
		}
		bbxSend(d, 2u);
	}
}
