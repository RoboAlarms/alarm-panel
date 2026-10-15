#pragma once
/* alarm_mem - where the panel's big, slow memory goes (docs/features/33 step 3, ADR-037).
 *
 * Internal RAM is the alarm's reserve (docs/bugs/B001): code hashing, the engine's queue and
 * anything an ISR or DMA touches need it. Big buffers, task-only statics and stacks of tasks
 * that never write flash therefore go "slow":
 *
 *   - on a chip with PSRAM (CONFIG_SPIRAM), to PSRAM and only there: a request PSRAM cannot
 *     meet fails instead of quietly draining internal RAM;
 *   - on a chip without PSRAM (the classic ESP32 boards), to internal RAM, the only RAM there.
 *
 * The one policy lives here, so a board without PSRAM needs no change in the services.
 * Anything an ISR or DMA touches, and the stack of any task that writes flash or NVS, must not
 * use these (with flash auto-suspend off, the cache is disabled during a write and a PSRAM
 * stack would fault). */
#include <stddef.h>
#include <stdint.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Statics only one task touches and too big for the reserve: PSRAM when the build puts .bss
 * there (CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY), ordinary .bss otherwise. */
#define ALARM_SLOW_BSS EXT_RAM_BSS_ATTR

/* The heap capabilities slow memory is taken with on this build. */
uint32_t alarm_mem_slow_caps(void);

/* Slow memory, no fallback on a chip with PSRAM (see above). Free with free(). */
void *alarm_mem_slow_malloc(size_t size);
void *alarm_mem_slow_calloc(size_t n, size_t size);

/* Slow memory when there is any, else internal RAM: for objects that must exist either way
 * (the zone hub, the devices registry). Free with free(). */
void *alarm_mem_prefer_slow_calloc(size_t n, size_t size);

/* A task whose stack is slow memory (never for a task that writes flash or NVS). Same
 * arguments as xTaskCreate, on any core. The task must never return. */
BaseType_t alarm_mem_task_create(TaskFunction_t fn, const char *name, uint32_t stack_bytes,
                                 void *arg, UBaseType_t priority, TaskHandle_t *out);

/* Whether this build has PSRAM for slow memory. */
static inline int alarm_mem_has_psram(void)
{
#if CONFIG_SPIRAM
    return 1;
#else
    return 0;
#endif
}

#ifdef __cplusplus
}
#endif
