#ifndef DASH_FONTS_H
#define DASH_FONTS_H

#include "lvgl.h"

/* ============================================================================
 * Barlow Condensed ExtraBold faces for the v2 dashboard hero numerals.
 *
 * Generated with lv_font_conv from Google Fonts' BarlowCondensed-ExtraBold
 * (SIL Open Font License 1.1), bpp 4, restricted to the glyphs a lap timer
 * actually shows:
 *
 *   0x2B '+'   0x2D '-'   0x2E '.'   0x30-0x39 digits   0x3A ':'
 *
 * That restriction is what keeps them affordable — the full Latin range at
 * 96 px would be an order of magnitude larger. Regenerate with:
 *
 *   npx lv_font_conv --font BarlowCondensed-ExtraBold.ttf \
 *       -r '0x2B,0x2D,0x2E,0x30-0x3A' --size 96 --format lvgl \
 *       --bpp 4 --no-compress -o font_delta_96.c
 *
 * Do NOT use these for anything containing letters: every non-numeric glyph
 * is absent and will render as a blank box.
 * ========================================================================= */

#ifdef __cplusplus
extern "C" {
#endif

LV_FONT_DECLARE(font_delta_96);   /* hero vs-best delta   */
LV_FONT_DECLARE(font_speed_72);   /* speed readout        */
LV_FONT_DECLARE(font_lap_60);     /* lap clock            */
LV_FONT_DECLARE(font_pill_44);    /* sector pill value    */

#ifdef __cplusplus
}
#endif

#endif /* DASH_FONTS_H */
