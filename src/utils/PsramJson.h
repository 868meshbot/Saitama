// Saitama — PsramJson.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// ArduinoJson allocator backed by PSRAM. The contacts/repeaters JSON for a
// full list (250 entries) runs to tens of KB; built in internal DRAM —
// shared with the BLE stack — it could run out part-way, and ArduinoJson
// then silently drops the rest, so a truncated list was written to SD.

#pragma once
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

namespace ops {

struct PsramJsonAllocator : ArduinoJson::Allocator {
    void* allocate(size_t size) override {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    }
    void deallocate(void* ptr) override {
        heap_caps_free(ptr);
    }
    void* reallocate(void* ptr, size_t newSize) override {
        return heap_caps_realloc(ptr, newSize, MALLOC_CAP_SPIRAM);
    }
};

inline ArduinoJson::Allocator* psramJson() {
    static PsramJsonAllocator a;
    return &a;
}

}  // namespace ops
