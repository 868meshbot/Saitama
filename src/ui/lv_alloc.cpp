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

// ── Core allocators ───────────────────────────────────────────────────────────
// Small allocations (< PSRAM_THRESHOLD) stay in internal DRAM.  LVGL's linked
// lists, style nodes, draw-task structs, and object fields are tiny but
// accessed very frequently — PSRAM latency breaks rendering for these.
// Large allocations (font bitmaps, image caches, widget render buffers) go to
// PSRAM so they don't exhaust the ~320 KB internal DRAM that FreeRTOS task
// stacks (MP3 player, etc.) depend on.
static constexpr size_t PSRAM_THRESHOLD = 1024;  // bytes

void* lv_malloc_core(size_t size)
{
    if (size >= PSRAM_THRESHOLD) {
        void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p) return p;
    }
    return malloc(size);  // small allocs or PSRAM full: use internal DRAM
}

void lv_free_core(void* p)
{
    free(p);
}

void* lv_realloc_core(void* p, size_t new_size)
{
    if (new_size >= PSRAM_THRESHOLD) {
        void* np = heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (np) return np;
    }
    return realloc(p, new_size);
}

// ── Monitor / test stubs ──────────────────────────────────────────────────────
void         lv_mem_monitor_core(lv_mem_monitor_t* /*mon_p*/) {}
lv_result_t  lv_mem_test_core(void) { return LV_RESULT_OK; }

} // extern "C"
