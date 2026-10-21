/*
 * failsafe.h - graded failsafe arbitration state machine for MiniFly
 * STM32F411 firmware.
 *
 * Arbitration layer over the already-existing protection primitives
 * (commander link timeout, anomal_detec free-fall/tumble, mag health
 * gate, pm low-battery). It does NOT replace them; it adds:
 *   1. a single observable state word (SWD/telemetry-readable),
 *   2. entry/exit hysteresis so one-shot sensor dropouts do not flap,
 *   3. the two actions the baseline never took:
 *        - KILL on commander.emerStop (the WiFi emergency-stop bit was
 *          write-only dead code),
 *        - forced auto-land on sustained low battery (pm only blinked
 *          the LED).
 *
 * Levels (monotonic arbitration, re-evaluated every 1kHz tick):
 *   NORMAL    0 -> everything healthy
 *   DEGRADED  1 -> fixed-point mode but mag/optical-flow lost: yaw/pos
 *                  hold degrades to gyro-only; warn, no forced action
 *   LAND      2 -> link lost (commander isRCLocked) or sustained low
 *                  battery: trigger commander one-key landing if airborne
 *   KILL      3 -> emerStop latch: cut motor PWM to zero immediately
 *                  via the powerControl hard gate
 *
 * Priority: KILL > LAND > DEGRADED > NORMAL. Each level exits only when
 * its own condition clears AND holds clear for the exit-hold time
 * (hysteresis). KILL is latched and clears only on disarm.
 *
 * Called once per millisecond from stabilizerTask (1kHz).
 * All state is static - zero dynamic allocation (project hard constraint).
 */
#ifndef __FAILSAFE_H
#define __FAILSAFE_H

#include <stdint.h>
#include <stdbool.h>

typedef enum
{
	FS_NORMAL   = 0,
	FS_DEGRADED = 1,
	FS_LAND     = 2,
	FS_KILL     = 3
} FailsafeLevel_e;

/* Condition bitmap bits (sticky while condition lasts). */
#define FS_COND_LINK_LOST   0x0001u	/* commander 1s shutdown timeout */
#define FS_COND_LOW_BAT     0x0002u	/* pm low-power flag */
#define FS_COND_MAG_LOST    0x0004u	/* mag health gate failed */
#define FS_COND_FLOW_LOST   0x0008u	/* optical-flow data stale */
#define FS_COND_EMER_STOP   0x0010u	/* WiFi emergency stop latched */

/* Called at 1kHz from stabilizerTask, AFTER commanderGetSetpoint/anomalDetec
 * and BEFORE stateControl(). Reads other modules via getters only. */
void failsafeUpdate(void);

/* Current level + condition bits, for telemetry/SWD inspection. */
uint8_t  failsafeLevel(void);
uint16_t failsafeConditionBits(void);

/* Emergency-stop latch request (set by WiFi ctrl path, consumed here). */
void failsafeRequestKill(bool kill);

/* powerControl() hard gate: true = zero all motor PWM. */
bool failsafeMotorCut(void);

/* "NORMAL"/"DEGRADED"/"LAND"/"KILL" for debug print. */
const char *failsafeLevelName(void);

#endif /* __FAILSAFE_H */
