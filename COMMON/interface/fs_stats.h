/*
 * fs_stats.h - run-time statistics reporter (DWT cycle counter + stack
 * high-water). See fs_stats.c for the clock-source contract.
 */
#ifndef __FS_STATS_H
#define __FS_STATS_H

#include <stdint.h>

/* Called by portCONFIGURE_TIMER_FOR_RUN_TIME_STATS() from
 * vTaskStartScheduler() before the first context switch. */
void fsStatsInit(void);

/* Called by portGET_RUN_TIME_COUNTER_VALUE() every context switch.
 * Returns a monotonically increasing (mod 2^32) cycle-derived count. */
unsigned long fsStatsCycles(void);

/* Create the periodic stats-report task (call once from startTask). */
void fsStatsStart(void);

#endif /* __FS_STATS_H */
