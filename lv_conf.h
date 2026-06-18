/* Saitama — LVGL 9.5 Configuration
 * Minimal config for T-Deck: 320x240, ESP32-S3, 8 MB PSRAM
 */
/* clang-format off */
#ifndef LV_CONF_H
#define LV_CONF_H

/*====================
   COLOR SETTINGS
 *====================*/
/** Color depth: 1(I1), 8(L8), 16(RGB565), 24(RGB888), 32(XRGB8888) */
#define LV_COLOR_DEPTH 16

/*=========================
   STDLIB WRAPPER SETTINGS
 *=========================*/
/* LV_STDLIB_CUSTOM: lv_malloc_core/lv_free_core/lv_realloc_core implemented
 * in src/ui/lv_alloc.cpp to route LVGL allocations to PSRAM, freeing internal
 * DRAM for FreeRTOS task stacks (MP3 player, etc.). */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CUSTOM
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB

#define LV_STDINT_INCLUDE   <stdint.h>
#define LV_STDDEF_INCLUDE   <stddef.h>
#define LV_STDBOOL_INCLUDE  <stdbool.h>
#define LV_INTTYPES_INCLUDE <inttypes.h>
#define LV_LIMITS_INCLUDE   <limits.h>
#define LV_STDARG_INCLUDE   <stdarg.h>

/*====================
   HAL SETTINGS
 *====================*/
#define LV_DEF_REFR_PERIOD  33      /* ~30 fps [ms] */
#define LV_DPI_DEF          130

/* LV_TICK_CUSTOM removed in LVGL 9 — tick source set via lv_tick_set_cb() in UIScreen.cpp */

/*=======================
   FEATURE CONFIGURATION
 *=======================*/
#define LV_USE_LOG 0

/* Widgets — LVGL 9 names */
#define LV_USE_ANIMIMG    1
#define LV_USE_ARC        1
#define LV_USE_BAR        1
#define LV_USE_BUTTON     1
#define LV_USE_BUTTONMATRIX 1
#define LV_USE_CANVAS     1
#define LV_USE_CHECKBOX   1
#define LV_USE_DROPDOWN   1
#define LV_USE_IMAGE      1
#define LV_USE_LABEL      1
#define LV_USE_LINE       1
#define LV_USE_LIST       1
#define LV_USE_MENU       0
#define LV_USE_MSGBOX     1
#define LV_USE_ROLLER     1
#define LV_USE_SCALE      0
#define LV_USE_SLIDER     1
#define LV_USE_SPAN       0
#define LV_USE_SPINBOX    0
#define LV_USE_SPINNER    1
#define LV_USE_SWITCH     1
#define LV_USE_TABVIEW    1
#define LV_USE_TABLE      0
#define LV_USE_TEXTAREA   1
#define LV_USE_TILEVIEW   0
#define LV_USE_WIN        0

/* Layouts */
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/* Themes */
#define LV_USE_THEME_DEFAULT 1
#define LV_USE_THEME_SIMPLE  1

/* Fonts */
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Enable RLE-compressed glyph bitmaps for the extended-Latin font variants */
#define LV_USE_FONT_COMPRESSED 1

/* Show a thin box for glyphs not present in any font in the fallback chain */
#define LV_USE_FONT_PLACEHOLDER 1

/* Image decoders */
#define LV_USE_PNG     1
#define LV_USE_LODEPNG 1

/* Disable platform-specific draw backends not available on ESP32-S3 */
#define LV_USE_DRAW_ARM2D    0
#define LV_USE_DRAW_HELIUM   0
#define LV_USE_DRAW_NEMA_GFX 0
#define LV_USE_DRAW_SDL      0
#define LV_USE_DRAW_VGLITE   0
#define LV_USE_DRAW_PXP      0
#define LV_USE_DRAW_DAVE2D   0
#define LV_USE_DRAW_EVE      0
#define LV_USE_DRAW_NANOVG   0
#define LV_USE_DRAW_OPENGLES 0
#define LV_USE_DRAW_DMA2D    0

/* Input devices */
#define LV_USE_GROUP 1

/* Image-font: maps Unicode codepoints to lv_image_dsc_t bitmaps (emoji) */
#define LV_USE_IMGFONT 1

/* Screenshot: render active screen to an off-screen buffer */
#define LV_USE_SNAPSHOT 1

#endif /* LV_CONF_H */
