/*
 * failsafe.c - graded failsafe arbitration state machine.
 * See failsafe.h for the level/priority contract.
 *
 * Runs inside stabilizerTask at 1kHz. Every input is a debounced getter
 * from an existing module (commander 1s link watchdog, pm 5s low-bat
 * watchdog, sensors presence flags, optical-flow data flag); this module
 * only arbitrates levels, adds DEGRADED hysteresis, and executes the two
 * actions the baseline lacked (emerStop consumption, low-bat auto-land).
 *
 * All storage static; no allocation, no ISR context.
 */
#include "failsafe.h"
#include "commander.h"
#include "stabilizer.h"
#include "sensors.h"
#include "pm.h"
#include "optical_flow.h"
#include "atkp.h"
#include "config_param.h"

/* ---- hysteresis timing (ms, module runs at 1kHz) ---- */
#define FS_DEG_ENTRY_MS      500u	/* mag/flow must be lost this long */
#define FS_DEG_EXIT_MS      2000u	/* ...and recovered this long to leave */
#define FS_LAND_EXIT_MS     1000u	/* land condition clear hold */

/* ---- condition raw sampling + per-level counters ----
 * level/condBits/motorCut are non-static on purpose: they are the
 * SWD-inspectable state words (addresses in the .map) for ground tests. */
uint8_t  failsafeLevelVar;
uint16_t failsafeCondBitsVar;
bool     failsafeMotorCutVar;

static uint32_t degLostCnt;		/* consecutive ms of degraded condition */
static uint32_t degOkCnt;		/* consecutive ms of degraded clear */
static uint32_t landOkCnt;		/* consecutive ms of land condition clear */
static bool     degActive;		/* hysteresis latched degraded */
static bool     landActive;		/* hysteresis latched land */

static const char *const levelNames[] = {"NORMAL", "DEGRADED", "LAND", "KILL"};

void failsafeRequestKill(bool kill)	/* kept for API symmetry; wifi path
					   uses setCommanderEmerStop instead */
{
	setCommanderEmerStop(kill);
}

/* Sample all inputs, maintain hysteresis counters, return condition bits. */
static uint16_t failsafeSampleConditions(void)
{
	uint16_t bits = 0;
	bool posMode;

	posMode = (getCommanderCtrlMode() & 0x03) == 0x03;	/* 定点模式 */

	if (commanderIsRCLocked())
		bits |= FS_COND_LINK_LOST;
	if (getIsLowpower())
		bits |= FS_COND_LOW_BAT;
	if (getCommanderEmerStop())
		bits |= FS_COND_EMER_STOP;

	/* DEGRADED only matters when a mode actually consumes the sensor:
	 * fixed-point needs flow (XY) and yaw-hold needs a usable mag
	 * (present + calibrated-valid, mirroring the fusion gate in
	 * stabilizer.c). In plain altitude/manual modes a dead mag is
	 * silently handled by the gyro-only fallback, so do not warn. */
	if (posMode && (!getOpDataState() || !sensorsIsMagPresent() ||
					!configParam.magCalibration.valid))
		bits |= FS_COND_MAG_LOST | FS_COND_FLOW_LOST;

	return bits;
}

void failsafeUpdate(void)
{
	uint16_t bits = failsafeSampleConditions();
	bool airborne = atkpIsFlightUnlocked();
	bool killCond = (bits & FS_COND_EMER_STOP) != 0;
	bool landCond = (bits & (FS_COND_LINK_LOST | FS_COND_LOW_BAT)) != 0;
	bool degCond  = (bits & (FS_COND_MAG_LOST | FS_COND_FLOW_LOST)) != 0;

	failsafeCondBitsVar = bits;

	/* ---- KILL: immediate entry, latched until disarmed ---- */
	if (killCond)
	{
		failsafeMotorCutVar = true;
		if (failsafeLevelVar != FS_KILL)
		{
			/* take over the commander state so nothing else
			 * keeps asking for thrust */
			setCommanderKeyFlight(false);
			setCommanderKeyland(false);
		}
		failsafeLevelVar = FS_KILL;
	}
	else if (failsafeMotorCutVar && !airborne)
	{
		/* emerStop released and craft on the ground: re-arm allowed */
		failsafeMotorCutVar = false;
	}

	if (failsafeMotorCutVar)
		return;	/* KILL dominates everything */

	/* ---- LAND: entry immediate (sources already debounced),
	 * exit needs landOkCnt hold after the condition clears ---- */
	if (landCond)
	{
		landOkCnt = 0;
		landActive = true;
		if (airborne && !getCommanderKeyland())
		{
			/* one-key land via the existing commander path;
			 * idempotent: only armed when actually flying */
			setCommanderKeyFlight(false);
			setCommanderKeyland(true);
		}
	}
	else if (landActive)
	{
		if (++landOkCnt >= FS_LAND_EXIT_MS)
		{
			landActive = false;
			landOkCnt = 0;
		}
	}

	/* ---- DEGRADED: symmetric hysteresis 500ms in / 2000ms out ---- */
	if (degCond)
	{
		degOkCnt = 0;
		if (!degActive && ++degLostCnt >= FS_DEG_ENTRY_MS)
			degActive = true;
	}
	else
	{
		degLostCnt = 0;
		if (degActive && ++degOkCnt >= FS_DEG_EXIT_MS)
		{
			degActive = false;
			degOkCnt = 0;
		}
	}

	/* ---- arbitration KILL > LAND > DEGRADED > NORMAL ---- */
	if (landActive)
		failsafeLevelVar = FS_LAND;
	else if (degActive)
		failsafeLevelVar = FS_DEGRADED;
	else
		failsafeLevelVar = FS_NORMAL;
}

uint8_t failsafeLevel(void)
{
	return failsafeLevelVar;
}

uint16_t failsafeConditionBits(void)
{
	return failsafeCondBitsVar;
}

bool failsafeMotorCut(void)
{
	return failsafeMotorCutVar;
}

const char *failsafeLevelName(void)
{
	return levelNames[failsafeLevelVar & 0x03];
}
