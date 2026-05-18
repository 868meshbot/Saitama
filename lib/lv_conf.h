/* Saitama LVGL 8.3 Configuration
 * Minimal config for T-Deck Plus: 320x240, ESP32-S3, 8MB PSRAM
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*=====================
   COLOR SETTINGS
 *=====================*/
#define LV_COLOR_DEPTH 16

/*=========================
   STDLIB / MEMORY
 *=========================*/
#define LV_STDLIB_INCLUDE <stdint.h>
#define LV_STDINT_INCLUDE <stdint.h>
#define LV_STDLIB_MALLOC       pvPortMalloc
#define LV_STDLIB_REALLOC      vPortFree
#define LV_STDLIB_FREE         vPortFree
/* Override: use PSRAM for large allocations */
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE <Arduino.h>
#define LV_MEM_CUSTOM_ALLOC   ps_malloc
#define LV_MEM_CUSTOM_FREE    free
#define LV_MEM_SIZE          (32U * 1024)  /* 32KB minimal pool, custom alloc handles the rest */

/*=====================
   HAL SETTINGS
 *=====================*/
#define LV_DISP_DEF_REFR_PERIOD  33    /* 30fps */
#define LV_INDEV_DEF_READ_PERIOD 30
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE <Arduino.h>
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

/*=======================
   FEATURE CONFIGURATION
 *=======================*/
#define LV_USE_LOG 0

/* Widgets */
#define LV_USE_ANIMIMG    1
#define LV_USE_ARC       1
#define LV_USE_BAR       1
#define LV_USE_BTN       1
#define LV_USE_BTNMATRIX 1
#define LV_USE_CANVAS    1
#define LV_USE_CHECKBOX  1
#define LV_USE_DROPDOWN  1
#define LV_USE_IMG       1
#define LV_USE_LABEL     1
#define LV_USE_LINE      1
#define LV_USE_LIST      1
#define LV_USE_MENU      0
#define LV_USE_MSGBOX    1
#define LV_USE_ROLLER    1
#define LV_USE_SCALE     0
#define LV_USE_SLIDER    1
#define LV_USE_SPAN      0
#define LV_USE_SPINBOX   0
#define LV_USE_SPINNER   1
#define LV_USE_SWITCH    1
#define LV_USE_TABVIEW   1
#define LV_USE_TABLE     0
#define LV_USE_TEXTAREA  1
#define LV_USE_TILEVIEW  0
#define LV_USE_WIN       0

/* Themes */
#define LV_USE_THEME_DEFAULT 1
#define LV_USE_THEME_SIMPLE  1

/* Font */
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Image decoders */
#define LV_USE_PNG 1

/* Disable platform-specific draw backends (not available on ESP32-S3) */
#define LV_USE_DRAW_ARM2D 0
#define LV_USE_DRAW_HELIUM 0
#define LV_USE_DRAW_NEMA_GFX 0
#define LV_USE_DRAW_SDL 0
#define LV_USE_DRAW_VGLITE 0
#define LV_USE_DRAW_PXP 0
#define LV_USE_DRAW_DAVE2D 0
#define LV_USE_DRAW_EVE 0
#define LV_USE_DRAW_NANOVG 0
#define LV_USE_DRAW_OPENGLES 0
#define LV_USE_DRAW_DMA2D 0

/* Input devices */
#define LV_USE_GROUP 1

/* Show a thin box for glyphs not present in any font in the fallback chain. */
#define LV_USE_FONT_PLACEHOLDER 1

/* Image-font: maps Unicode codepoints to small lv_img_dsc_t bitmaps (used for emoji). */
#define LV_USE_IMGFONT 1

#endif /* LV_CONF_H */