// Saitama — lv_alloc.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// LVGL 9 custom allocator (LV_STDLIB_CUSTOM) — routes lv_malloc/free/realloc
// to PSRAM so LVGL widget + style allocations don't eat internal DRAM.
// Internal DRAM is left for FreeRTOS task stacks (MP3 player etc.).

#include <Arduino.h>
#include <esp_heap_caps.h>

extern "C" {

void* lv_malloc_core(size_t size)
{
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(size);  // fallback to internal DRAM
    return p;
}

void lv_free_core(void* p)
{
    free(p);
}

void* lv_realloc_core(void* p, size_t new_size)
{
    void* np = heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!np) np = realloc(p, new_size);  // fallback
    return np;
}

} // extern "C"
