// Saitama — ops_lv_alloc.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// LVGL's allocator on boards without PSRAM (lv_conf.h, LV_MEM_CUSTOM_ALLOC).
// Plain C so the LVGL library can call it; defined in src/utils/LvMem.cpp.

#pragma once
#include <stddef.h>
#include <stdlib.h>   // free(): LV_MEM_CUSTOM_FREE

#ifdef __cplusplus
extern "C" {
#endif

void* ops_lv_malloc(size_t size);
void* ops_lv_realloc(void* p, size_t size);

#ifdef __cplusplus
}
#endif
