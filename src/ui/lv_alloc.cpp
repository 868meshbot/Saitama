// Saitama — lv_alloc.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// LVGL 9 custom allocator (LV_STDLIB_CUSTOM) — routes lv_malloc/free/realloc
// to PSRAM so LVGL widget + style allocations don't eat internal DRAM.
// Internal DRAM is left for FreeRTOS task stacks (MP3 player etc.).

#include <Arduino.h>
#include <esp_heap_caps.h>

// lv_mem.h types needed for monitor/test stubs
#include <lvgl.h>

extern "C" {

// ── Lifecycle ────────────────────────────────────────────────────────────────
void lv_mem_init(void)   { /* PSRAM is always ready on ESP32-S3 */ }
void lv_mem_deinit(void) {}

// ── Pool management (not supported with heap_caps) ───────────────────────────
lv_mem_pool_t lv_mem_add_pool(void* /*mem*/, size_t /*bytes*/) { return nullptr; }
void          lv_mem_remove_pool(lv_mem_pool_t /*pool*/)       {}

// ── Core allocators — route to PSRAM, fall back to internal DRAM ─────────────
void* lv_malloc_core(size_t size)
{
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(size);
    return p;
}

void lv_free_core(void* p)
{
    free(p);
}

void* lv_realloc_core(void* p, size_t new_size)
{
    void* np = heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!np) np = realloc(p, new_size);
    return np;
}

// ── Monitor / test stubs ──────────────────────────────────────────────────────
void         lv_mem_monitor_core(lv_mem_monitor_t* /*mon_p*/) {}
lv_result_t  lv_mem_test_core(void) { return LV_RESULT_OK; }

} // extern "C"
