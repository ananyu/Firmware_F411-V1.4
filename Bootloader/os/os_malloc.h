/*
 * os_malloc.h stub for MiniFly MCUBoot port (AC5 / bare-metal).
 *
 * Upstream loader.c includes <os/os_malloc.h> unconditionally but never
 * calls os_malloc/os_free in the swap-scratch path. Map them to stdlib
 * just in case a future config needs them.
 */

#ifndef __OS_MALLOC_H
#define __OS_MALLOC_H

#include <stdlib.h>

#define os_malloc   malloc
#define os_calloc   calloc
#define os_realloc  realloc
#define os_free     free

#endif /* __OS_MALLOC_H */
