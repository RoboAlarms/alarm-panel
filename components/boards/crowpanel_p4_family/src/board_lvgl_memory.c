/* LVGL objects, styles, strings and software drawing scratch belong in PSRAM, including
 * allocations smaller than the general malloc threshold. Do not fall back to internal RAM:
 * a large UI page must not consume the memory reserved for alarm tasks, drivers and crypto.
 * Display DMA buffers are allocated separately by esp_lvgl_port with their required caps.
 * The cache-safe frame ISR does not access LVGL; the DMA2D completion ISR is not cache-safe.
 */
#include "lvgl.h"
#include "esp_heap_caps.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM
void lv_mem_init(void) {}
void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    (void)mem;
    (void)bytes;
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { (void)pool; }

void *lv_malloc_core(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void *lv_realloc_core(void *ptr, size_t size)
{
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void lv_free_core(void *ptr) { heap_caps_free(ptr); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon)
{
    /* PSRAM is shared with other services; these are heap-wide figures. */
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    mon->total_size = info.total_free_bytes + info.total_allocated_bytes;
    mon->free_size = info.total_free_bytes;
    mon->free_biggest_size = info.largest_free_block;
    mon->free_cnt = info.free_blocks;
    mon->used_cnt = info.allocated_blocks;
    mon->max_used = mon->total_size - info.minimum_free_bytes;
    mon->used_pct = mon->total_size ? 100u - (100u * mon->free_size / mon->total_size) : 0;
    mon->frag_pct = mon->free_size ? 100u - (100u * mon->free_biggest_size / mon->free_size) : 0;
}

lv_result_t lv_mem_test_core(void)
{
    return heap_caps_check_integrity(MALLOC_CAP_SPIRAM, true) ? LV_RESULT_OK : LV_RESULT_INVALID;
}
#endif
