#pragma once
/* Panel Manager scratch memory. Internal RAM is the alarm's reserve: code hashing failed
 * when it ran short (docs/bugs/B001). Everything the web service builds per request (JSON trees,
 * configuration copies, log pages) lives in slow memory (alarm_mem: PSRAM, with no silent
 * fallback to internal RAM): a request that can't get memory is refused instead. Task
 * stacks stay internal, because the HTTPS task writes flash (uploads, NVS). */
#include "alarm_mem.h"
#include <stddef.h>

static inline void *web_alloc(size_t size)
{
    return alarm_mem_slow_calloc(1, size);
}
