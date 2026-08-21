#ifndef DASH_THEME_H
#define DASH_THEME_H

#include <stdint.h>

/* ============================================================================
 * The dashboard palette, and nothing else.
 *
 * A leaf header on purpose. uiHelper.h owns the setter API and therefore pulls
 * in esp_bsp.h, ui.h, ui_dash2.h, LapManager.h, display.h and lv_port.h — far
 * too much for a hand-built screen that only wants four colours, which is why
 * SessionBrowser and LogScreen each grew their own hand-copied palette instead.
 * One of those copies had already drifted (a constant named `surface` holding
 * `surface2`'s value), so the colours live here where including them is cheap
 * and a retune cannot miss a copy.
 * ========================================================================= */

typedef enum {
    DASH_MODE_NIGHT = 0,    /* black bg, amber accent (default) */
    DASH_MODE_DAY   = 1,    /* black bg, cyan accent for daylight LCD pop */
} dash_mode_t;

typedef struct {
    uint32_t bg, surface, surface2, fg, fg2, muted, rule, accent, accent_fg, good, bad, good_deep, bad_deep;
} dash_theme_t;

static const dash_theme_t THEME_NIGHT = {
    .bg = 0x050608, .surface = 0x0D1014, .surface2 = 0x14181E, .fg = 0xF6F8FB,
    .fg2 = 0xCBD0D8, .muted = 0x6B7280, .rule = 0x1C2026, .accent = 0xFFD400,
    .accent_fg = 0x1A1500, .good = 0x2EE07A, .bad = 0xFF3B3B,
    .good_deep = 0x0F3A23, .bad_deep = 0x3A0F10
};
static const dash_theme_t THEME_DAY = {
    .bg = 0xF4F5F7, .surface = 0xFFFFFF, .surface2 = 0xECEEF2, .fg = 0x14161A,
    .fg2 = 0x3A3F47, .muted = 0x9AA3AF, .rule = 0xD8DBE1, .accent = 0xFF9500,
    .accent_fg = 0x1A0E00, .good = 0x29FF8A, .bad = 0xFF5050,
    .good_deep = 0x0A5933, .bad_deep = 0x3A2425
};

#endif // DASH_THEME_H
