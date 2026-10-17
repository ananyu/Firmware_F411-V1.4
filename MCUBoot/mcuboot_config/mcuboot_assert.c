/*
 * mcuboot_assert.c - C library assert hook for the app-side MCUBoot code.
 *
 * ARMCC routes assert() failures to __aeabi_assert(); the FW has no
 * C-library handler but does have assertFail() (debug_assert.c), which
 * blocks until the IWDG resets the MCU. Behaves the same as the
 * bootloader-side choice of keeping asserts live.
 */
#include "debug_assert.h"

void __aeabi_assert(const char *expr, const char *file, int line)
{
    assertFail((char *)expr, (char *)file, line);
}
