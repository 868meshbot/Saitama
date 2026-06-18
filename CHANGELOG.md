# Changelog — Saitama

All notable changes to this project. Dates are UTC.

---

## [1.1.0] — 2026-06-18  *(lvgl9 branch)*

### Changed
- **LVGL 8.3.4 → LVGL 9.5.0** — complete UI framework upgrade.  All source
  files migrated to LVGL 9 APIs; see migration notes below.
- **NavBoxLib** updated to `main` branch (targets `lvgl/lvgl >= 9.0.0`),
  eliminating the old `lvgl@8.4.0` transitive dependency that shadowed the
  LVGL 9 headers.
- **LVGL allocator** (`src/ui/lv_alloc.cpp`) routes large (≥ 1 KB) LVGL
  allocations to PSRAM, freeing ≈ 100 KB of internal DRAM for FreeRTOS task
  stacks (MP3 player, etc.). Small allocations stay in DRAM for LVGL 9
  internal list nodes and draw-task structures.
- **Spectrum canvas** buffer changed from `lv_color_t*` (3-byte RGB888 in
  LVGL 9) to `uint16_t*` (RGB565); all pixel writes use `lv_color_to_u16()`.
- **Map tile loading** fixed: NavBoxLib queues tile requests asynchronously
  when PSRAM is present (`cropmode_ = false`); `ScreenMap::tick()` now calls
  `s_renderer.iterate()` every frame to drain the queue.

### Fixed
- Boot screen frozen — `LV_TICK_CUSTOM` was silently dropped in LVGL 9;
  tick source now registered via `lv_tick_set_cb(millis)` after `lv_init()`.
- MP3 player task-create failure due to DRAM exhaustion (fixed by PSRAM
  routing above).
- `lv_event_send(obj, code, data)` → `lv_obj_send_event()` (10 call sites).
- `lv_event_get_target()` returns `void*` in LVGL 9; cast to `lv_obj_t*`.
- `lv_spinner_create()` no longer takes time/arc params; use
  `lv_spinner_set_anim_params()` separately.
- `lv_obj_set_style_size()` gains separate width/height args.
- `lv_imgfont_create` callback signature changed: returns `const void*`
  pointer to image descriptor (not write-into-buffer), adds `user_data` arg.
- `LV_USE_ANIMIMAGE` renamed to `LV_USE_ANIMIMG` internally; conf updated.
- `LV_USE_LODEPNG 1` required for screenshot `lodepng_encode24`.
- Bitwise OR of `lv_obj_flag_t` / `lv_border_side_t` now requires explicit
  cast to the enum type.
- `lv_color_t.full` field removed in LVGL 9; use `lv_color_to_u16()`.
- NavBoxLib tile-queue drain: `iterate()` called each tick via
  `ScreenMap::tick()` wired into `UIScreen::tick()`.

### LVGL 9 migration notes
| LVGL 8 | LVGL 9 |
|---|---|
| `lv_disp_drv_t` + `lv_disp_drv_register()` | `lv_display_create()` |
| `lv_indev_drv_t` + `lv_indev_drv_register()` | `lv_indev_create()` |
| `lv_disp_flush_ready(drv)` | `lv_display_flush_ready(disp)` |
| `LV_TICK_CUSTOM` in lv_conf.h | `lv_tick_set_cb(cb)` after `lv_init()` |
| `lv_event_send(obj, code, data)` | `lv_obj_send_event(obj, code, data)` |
| `lv_event_get_target()` → `lv_obj_t*` | cast: `(lv_obj_t*)lv_event_get_target()` |
| `lv_canvas_draw_arc/line/rect/text()` | `lv_canvas_init_layer()` + `lv_draw_*()` + `lv_canvas_finish_layer()` |
| `lv_color_t.full` (uint16) | `lv_color_to_u16(color)` |
| `lv_color_t` is RGB565 (2 B) | `lv_color_t` is RGB888 (3 B); canvas buffers must be `uint16_t*` |
| `lv_spinner_create(parent, t, arc)` | `lv_spinner_create(parent)` + `lv_spinner_set_anim_params()` |
| `lv_img_set_zoom(obj, 256)` | `lv_image_set_scale(obj, 256)` (compat alias works) |
| `LV_IMG_CF_TRUE_COLOR` | `LV_COLOR_FORMAT_NATIVE` |
| `lv_mem_alloc()` | `lv_malloc()` |

---

## [1.0.0] — 2026-06-11

### Added
- **NavBoxLib map renderer** — replaces hand-rolled `MapEngine` tile renderer
  with NavBoxLib's managed LVGL image-object tile cache, MarkerLayer overlay,
  and floating name labels via `renderer.project()`.
- **MeshCore 1.16.0** — preamble fix for SF ≤ 8, extended ACKs,
  `flood.max.unscoped`, anonymous requests, trace offset fix.

### Changed
- Channel Scan and Trace swapped on the launcher grid.
- Version string promoted to 1.0.0 (first stable release).

### Contributors
See `CONTRIBUTORS` file.
