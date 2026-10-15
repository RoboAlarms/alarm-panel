#include "alarm_mem.h"

#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"

uint32_t alarm_mem_slow_caps(void)
{
#if CONFIG_SPIRAM
    return MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#else
    return MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
}

void *alarm_mem_slow_malloc(size_t size)
{
    return heap_caps_malloc(size, alarm_mem_slow_caps());
}

void *alarm_mem_slow_calloc(size_t n, size_t size)
{
    return heap_caps_calloc(n, size, alarm_mem_slow_caps());
}

void *alarm_mem_prefer_slow_calloc(size_t n, size_t size)
{
    void *p = heap_caps_calloc(n, size, alarm_mem_slow_caps());
    return p != NULL ? p : heap_caps_calloc(n, size, MALLOC_CAP_DEFAULT);
}

BaseType_t alarm_mem_task_create(TaskFunction_t fn, const char *name, uint32_t stack_bytes,
                                 void *arg, UBaseType_t priority, TaskHandle_t *out)
{
    return xTaskCreatePinnedToCoreWithCaps(fn, name, stack_bytes, arg, priority, out,
                                           tskNO_AFFINITY, alarm_mem_slow_caps());
}
