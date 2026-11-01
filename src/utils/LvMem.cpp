// Saitama — LvMem.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "LvMem.h"
#include "../features.h"
#include <ops_lv_alloc.h>
#include <esp_heap_caps.h>
#include <stdlib.h>

namespace ops { namespace lvmem {

static void*             s_reserve   = nullptr;
static volatile bool     s_hit       = false;
static volatile uint32_t s_hits      = 0;
static volatile uint32_t s_failed    = 0;
static volatile size_t   s_failSize  = 0;

// Gives the reserve back to the heap. Returns true if there was one.
static bool _spendReserve()
{
    if (!s_reserve) return false;
    free(s_reserve);
    s_reserve = nullptr;
    s_hit = true;
    s_hits = s_hits + 1;
    return true;
}

#if !OPS_HAS_PSRAM
// Called by ESP-IDF on any failed heap allocation (LVGL, JSON, BLE...).
// Runs inside the failing malloc: only record, never allocate or log.
static void _onAllocFailed(size_t size, uint32_t /*caps*/, const char* /*fn*/)
{
    s_failed   = s_failed + 1;
    s_failSize = size;
}
#endif

void init()
{
#if !OPS_HAS_PSRAM
    heap_caps_register_failed_alloc_callback(_onAllocFailed);
    if (!s_reserve) s_reserve = malloc(RESERVE_BYTES);
#endif
}

bool lowMemHit()
{
    if (!s_hit) return false;
    s_hit = false;
    return true;
}

void restoreReserve()
{
#if !OPS_HAS_PSRAM
    // Only when there's clearly room: holding it must not starve what runs.
    if (s_reserve || largestFree() < RESERVE_BYTES + OPEN_MIN_BLOCK) return;
    s_reserve = malloc(RESERVE_BYTES);
#endif
}

bool     reserveHeld()    { return s_reserve != nullptr; }
uint32_t hits()           { return s_hits; }
uint32_t failedAllocs()   { return s_failed; }
size_t   lastFailedSize() { return s_failSize; }

size_t largestFree()
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

}}  // namespace ops::lvmem

extern "C" void* ops_lv_malloc(size_t size)
{
    void* p = malloc(size);
    if (!p && size && ops::lvmem::_spendReserve()) p = malloc(size);
    return p;
}

extern "C" void* ops_lv_realloc(void* old, size_t size)
{
    void* p = realloc(old, size);
    if (!p && size && ops::lvmem::_spendReserve()) p = realloc(old, size);
    return p;
}
