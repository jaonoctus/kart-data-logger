/* ============================================================================
 * ui_dash2.h — Dash v2 (lap-delta hero + sector band), LVGL 9
 *
 * Adopted from the Claude Design project "kart-data-logger"
 * (lvgl-port/ui_dash2.{h,c}, port of "Dash Final v2.html"). Local changes are
 * marked LOCAL: so a future design sync can be diffed against upstream:
 *
 *   LOCAL: the status bar is NOT built here. This firmware keeps a persistent
 *          bar on lv_layer_top() so it survives across the config, track and
 *          sessions screens, and it carries CAM / WIFI / REC cells upstream
 *          knows nothing about. Geometry is unchanged — upstream's bar is also
 *          26 px at y=0, so everything below still lands where the mock says.
 *   LOCAL: hero fonts point at the generated Barlow Condensed faces.
 * ========================================================================= */
#ifndef UI_DASH2_H
#define UI_DASH2_H

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#if LVGL_VERSION_MAJOR < 9
#error "ui_dash2 targets LVGL 9.x"
#endif

/* LOCAL: real faces rather than the montserrat fallbacks. Digits, ':', '.',
 * '+' and '-' only — see lib/DashFonts/DashFonts.h. Never put letters in a
 * label using these. */
#include "DashFonts.h"
#define DASH2_FONT_DELTA  &font_delta_96
#define DASH2_FONT_SPEED  &font_speed_72
#define DASH2_FONT_LAP    &font_lap_60
#define DASH2_FONT_PILL   &font_pill_44

typedef enum {
    DASH2_MODE_NIGHT = 0,
    DASH2_MODE_DAY   = 1,
} dash2_mode_t;

typedef enum {
    DASH2_S1  = 0,
    DASH2_S2  = 1,
    DASH2_END = 2,
    DASH2_SECTOR_COUNT = 3,
} dash2_sector_t;

typedef enum {
    DASH2_SECTOR_PENDING = 0,
    DASH2_SECTOR_ACTIVE,
    DASH2_SECTOR_FASTER,
    DASH2_SECTOR_SLOWER,
} dash2_sector_state_t;

/* Build the dash on `parent` (NULL = active screen). Call once. */
void ui_dash2_init(lv_obj_t *parent);

void ui_dash2_set_speed(int kmh);
void ui_dash2_set_lap(int lap_num, const char *lap_time, const char *best_time);
void ui_dash2_set_delta(float seconds);          /* negative = faster = green */

void ui_dash2_enter_sector(dash2_sector_t s);
void ui_dash2_close_sector(dash2_sector_t s, float delta_seconds);
void ui_dash2_set_running_split(const char *split);
void ui_dash2_reset_sectors(void);
void ui_dash2_set_sector(dash2_sector_t s, dash2_sector_state_t st, float delta_seconds);

void ui_dash2_set_mode(dash2_mode_t mode);
dash2_mode_t ui_dash2_get_mode(void);

/* LOCAL: the speed readout doubles as the way into the setup menu, as it did
 * on the screen this replaces. Upstream has no navigation of its own, so the
 * firmware attaches its own handler here rather than reaching into statics. */
lv_obj_t *ui_dash2_get_speed_obj(void);

#ifdef __cplusplus
}
#endif
#endif /* UI_DASH2_H */
