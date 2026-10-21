/*
 * fs_stats.c - run-time statistics + stack high-water reporter.
 *
 * Backs the FreeRTOS configGENERATE_RUN_TIME_STATS clock with the Cortex-M
 * DWT cycle counter (no extra timer peripheral, no extra RAM). The kernel
 * calls portCONFIGURE_TIMER_FOR_RUN_TIME_STATS() from vTaskStartScheduler
 * before the first context switch, which routes here to fsStatsInit().
 *
 * A single low-priority task periodically snapshots uxTaskGetSystemState()
 * (per-task CPU% + stack high-water) and prints it over the retargeted
 * printf (USART1). The numbers feed the static stack budgets in main.c: a
 * task that has never dipped below N free words can be trimmed toward N,
 * and a task pegging the CPU shows up as a WCET outlier.
 *
 * uxTaskGetSystemState is used instead of vTaskGetRunTimeStats because the
 * latter formats into a heap buffer; we keep one static snapshot array -
 * zero dynamic allocation (project rule).
 *
 * DWT notes for STM32F411 (Cortex-M4):
 *   DEMCR.TRCEENA must be set before the cycle counter runs.
 *   CYCCNT is a free-running 32-bit count of CPU cycles at configCPU_CLOCK_HZ
 *   (96MHz here); it wraps ~every 44.7s, which is fine because the kernel
 *   only ever looks at per-task deltas between context switches.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "freertos_static.h"
#include <stdint.h>
#include <stdio.h>
#include "fs_stats.h"

/* Period between reports, in ms. Long enough that the printf burst does not
 * disturb the flight loop noticeably, short enough to catch a regression. */
#define FS_REPORT_PERIOD_MS 5000UL

/* Max tasks we will snapshot in one pass (project currently runs ~13). */
#define FS_MAX_TASKS 20

/* DWT / DEMCR registers (ARMv7-M core peripheral, fixed addresses). */
#define FS_REG_DEMCR        (*(volatile uint32_t *)0xE000EDFCUL)
#define FS_REG_DWT_CTRL     (*(volatile uint32_t *)0xE0001000UL)
#define FS_REG_DWT_CYCCNT   (*(volatile uint32_t *)0xE0001004UL)
#define FS_DEMCR_TRCENA     (1UL << 24)
#define FS_DWT_CTRL_CYCCNTENA (1UL << 0)

void fsStatsInit(void)
{
	FS_REG_DEMCR |= FS_DEMCR_TRCENA;	/* trace block clock */
	FS_REG_DWT_CYCCNT = 0U;
	FS_REG_DWT_CTRL |= FS_DWT_CTRL_CYCCNTENA;
}

unsigned long fsStatsCycles(void)
{
	/* Right-shift by 6: 96MHz/64 = 1.5MHz count rate, 32-bit wrap every
	 * ~2860s (~47min) instead of 44.7s. Resolution 0.67us is far finer
	 * than any context-switch delta we care about, and the kernel's
	 * per-task accumulation + % math is unit-invariant (both sides use
	 * the same counter), so CPU% stays correct. */
	return (unsigned long)(FS_REG_DWT_CYCCNT >> 6);
}

/* Static snapshot buffer - no heap (configSUPPORT_STATIC_ALLOCATION rule). */
static TaskStatus_t s_taskStatus[FS_MAX_TASKS];

static void fsStatsTask(void *param)
{
	uint32_t totalRunTime;
	UBaseType_t n;
	UBaseType_t i;

	(void)param;

	for (;;)
	{
		vTaskDelay(pdMS_TO_TICKS(FS_REPORT_PERIOD_MS));

		n = uxTaskGetSystemState(s_taskStatus, FS_MAX_TASKS, &totalRunTime);

		printf("\n[FS_STATS] tasks=%u total=%lu\r\n", (unsigned)n,
				(unsigned long)totalRunTime);
		printf("[FS_STATS] name         pri  cpu%%  stackFree(w)\r\n");

		for (i = 0U; i < n; i++)
		{
			unsigned pct = 0U;
			if (totalRunTime > 0U)
			{
				pct = (unsigned)(((uint64_t)s_taskStatus[i].ulRunTimeCounter *
						100U) / totalRunTime);
			}
			printf("[FS_STATS] %-12s %2u  %3u%%  %4u\r\n",
					s_taskStatus[i].pcTaskName,
					(unsigned)s_taskStatus[i].uxCurrentPriority,
					pct,
					(unsigned)s_taskStatus[i].usStackHighWaterMark);
		}
		printf("[FS_STATS] ----\r\n");
	}
}

void fsStatsStart(void)
{
	DEFINE_STATIC_TASK(fsStats, 200);
	xTaskCreateStatic(fsStatsTask, "FS_STATS", 200, NULL, 1,
			stk_fsStats, &tcb_fsStats);
}
