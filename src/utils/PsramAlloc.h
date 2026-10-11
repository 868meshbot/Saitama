// Saitama — PsramAlloc.h
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Moves large static buffers out of internal DRAM. Internal RAM is what BLE,
// AES and the SD driver allocate from at runtime; when it runs out, BLE
// pairing fails ("BLE_INIT: Malloc failed") and channel packets fail to
// decrypt ("esp-aes: Failed to allocate memory"). The prebuilt Arduino core
// can't place .bss in PSRAM, so big arrays are bound to PSRAM storage instead:
//
//     static Foo (&s_foo)[N] = psram::array<Foo[N]>();
//
// Zeroed (like .bss) and never freed. sizeof(s_foo) and indexing work as
// before. Falls back to internal RAM if PSRAM is missing.

#pragma once
#include <esp_heap_caps.h>
#include <stdlib.h>
#include <type_traits>

namespace ops {
namespace psram {

template <typename T>
T& array()
{
    static_assert(std::is_trivially_default_constructible<
                      typename std::remove_all_extents<T>::type>::value,
                  "psram::array zero-fills; use it only for plain data");
    void* m = heap_caps_calloc(1, sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m) m = calloc(1, sizeof(T));
    return *static_cast<T*>(m);
}

}  // namespace psram
}  // namespace ops
