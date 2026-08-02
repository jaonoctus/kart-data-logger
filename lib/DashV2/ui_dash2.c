/* ============================================================================
 * ui_dash2.c — see ui_dash2.h. LVGL 9 · 480x320
 *
 * Layout (px, matching Dash Final v2.html):
 *   0    ─ status bar, 26 px ─ LOCAL: built elsewhere, on lv_layer_top()
 *   34   SPEED caption          128  hero band, 104 px
 *   48   speed value            242  sector band, 64 px
 *   40   LAP / BEST (right)     306  ─ end
 *
 * Two deliberate deviations from the web mock, both upstream:
 *  1. The hero's "VS BEST" caption is horizontal — LVGL labels have no
 *     vertical writing mode and a rotated canvas is not worth the RAM.
 *  2. Delta glyphs use LV_SYMBOL_UP / LV_SYMBOL_DOWN rather than ▲ / ▼.
 * ============================================================================ */
#include "ui_dash2.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

LV_FONT_DECLARE(lv_font_montserrat_12)
LV_FONT_DECLARE(lv_font_montserrat_14)
LV_FONT_DECLARE(lv_font_montserrat_16)

#define SCR_W        480
#define SCR_H        320
#define PAD_X         14
#define BAR_H         26
#define HERO_Y       128
#define HERO_H       104
#define BAND_Y       242
#define BAND_H        64
#define RAIL_W       146
#define BAND_GAP       8
#define CHIP_GAP       5
#define CONTENT_W    (SCR_W - PAD_X * 2)
#define PILL_W       (CONTENT_W - RAIL_W - BAND_GAP)

typedef struct {
    uint32_t bg, fg, muted, rule, track, accent;
    uint32_t good, bad, warn, good_deep, bad_deep;
} dash2_tokens_t;

static const dash2_tokens_t TOK_NIGHT = {
    .bg = 0x050608, .fg = 0xF6F8FB, .muted = 0x6B7280, .rule = 0x1C2026,
    .track = 0x1A1D23, .accent = 0xFFD400, .good = 0x2EE07A, .bad = 0xFF3B3B,
    .warn = 0xFFB020, .good_deep = 0x0F3A23, .bad_deep = 0x3A0F10,
};
static const dash2_tokens_t TOK_DAY = {
    .bg = 0x050608, .fg = 0xFFFFFF, .muted = 0x9AA3AF, .rule = 0x2A2F37,
    .track = 0x20242C, .accent = 0x00E5FF, .good = 0x29FF8A, .bad = 0xFF5050,
    .warn = 0xFFC940, .good_deep = 0x0F3A23, .bad_deep = 0x3A0F10,
};

static dash2_tokens_t T = {0};
static dash2_mode_t   s_mode = DASH2_MODE_NIGHT;

static const char *SECTOR_TAG[DASH2_SECTOR_COUNT] = { "S1", "S2", "END" };

typedef struct { dash2_sector_state_t state; float delta; } sector_slot_t;

static sector_slot_t s_sectors[DASH2_SECTOR_COUNT];
static int   s_current   = -1;
static float s_lap_delta = 0.0f;
static char  s_running[16] = "--.--";

static lv_obj_t *scr;
static lv_obj_t *lbl_speed_cap, *lbl_speed_v, *lbl_speed_unit;
static lv_obj_t *box_lap, *lbl_lap_n, *lbl_lap_v, *lbl_best;
static lv_obj_t *hero, *lbl_hero_cap, *lbl_hero_arrow, *lbl_hero_v, *lbl_hero_s;
static lv_obj_t *pill, *lbl_pill_tag, *lbl_pill_sub, *lbl_pill_v, *lbl_pill_s, *pill_dot;
static lv_obj_t *rail, *chip[DASH2_SECTOR_COUNT];
static lv_obj_t *chip_tag[DASH2_SECTOR_COUNT], *chip_val[DASH2_SECTOR_COUNT];

static inline lv_color_t C(uint32_t hex) { return lv_color_hex(hex); }

static void refresh_hero(void);
static void refresh_band(void);
static void apply_theme(void);

static lv_obj_t *panel(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static lv_obj_t *caption(lv_obj_t *parent, const char *txt, int track_px)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l, C(T.muted), 0);
    lv_obj_set_style_text_letter_space(l, track_px, 0);
    return l;
}

static void anim_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}
static void start_blink(lv_obj_t *obj, uint32_t period_ms)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_20);
    lv_anim_set_duration(&a, period_ms / 2);
    lv_anim_set_playback_duration(&a, period_ms / 2);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

void ui_dash2_init(lv_obj_t *parent)
{
    if (!parent) parent = lv_screen_active();
    scr    = parent;
    T      = TOK_NIGHT;
    s_mode = DASH2_MODE_NIGHT;

    for (int i = 0; i < DASH2_SECTOR_COUNT; i++) {
        s_sectors[i].state = DASH2_SECTOR_PENDING;
        s_sectors[i].delta = 0.0f;
    }

    lv_obj_set_size(scr, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(scr, C(T.bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* LOCAL: no status bar here — see the header. The 26 px it would occupy is
     * covered by the persistent bar on lv_layer_top(), so every y below is
     * still the mock's value. */

    /* ---------- SPEED (top-left) ---------- */
    lbl_speed_cap = caption(scr, "SPEED", 2);
    lv_obj_set_pos(lbl_speed_cap, PAD_X, 34);

    lbl_speed_v = lv_label_create(scr);
    lv_label_set_text(lbl_speed_v, "0");
    lv_obj_set_style_text_font(lbl_speed_v, DASH2_FONT_SPEED, 0);
    lv_obj_set_style_text_color(lbl_speed_v, C(T.fg), 0);
    lv_obj_set_pos(lbl_speed_v, PAD_X, 48);

    lbl_speed_unit = caption(scr, "KM/H", 2);
    lv_obj_align_to(lbl_speed_unit, lbl_speed_v, LV_ALIGN_OUT_RIGHT_BOTTOM, 7, -8);

    /* ---------- LAP (top-right) ---------- */
    box_lap = panel(scr);
    lv_obj_set_size(box_lap, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box_lap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box_lap, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(box_lap, 9, 0);
    lbl_lap_n = caption(box_lap, "LAP 0", 2);
    lbl_lap_v = lv_label_create(box_lap);
    lv_label_set_text(lbl_lap_v, "0:00.00");
    lv_obj_set_style_text_font(lbl_lap_v, DASH2_FONT_LAP, 0);
    lv_obj_set_style_text_color(lbl_lap_v, C(T.fg), 0);
    lv_obj_align(box_lap, LV_ALIGN_TOP_RIGHT, -PAD_X, 40);

    lbl_best = lv_label_create(scr);
    lv_label_set_text(lbl_best, "BEST --");
    lv_obj_set_style_text_font(lbl_best, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_best, C(T.muted), 0);
    lv_obj_align_to(lbl_best, box_lap, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 3);

    /* ---------- HERO — vs-best lap delta ---------- */
    hero = panel(scr);
    lv_obj_set_size(hero, CONTENT_W, HERO_H);
    lv_obj_set_pos(hero, PAD_X, HERO_Y);
    lv_obj_set_style_radius(hero, 8, 0);
    lv_obj_set_style_bg_opa(hero, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hero, 1, 0);
    lv_obj_set_style_pad_hor(hero, 20, 0);

    lbl_hero_cap = lv_label_create(hero);
    lv_label_set_text(lbl_hero_cap, "VS BEST");
    lv_obj_set_style_text_font(lbl_hero_cap, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(lbl_hero_cap, 3, 0);
    lv_obj_set_style_opa(lbl_hero_cap, LV_OPA_70, 0);
    lv_obj_align(lbl_hero_cap, LV_ALIGN_TOP_LEFT, 0, 14);

    lbl_hero_arrow = lv_label_create(hero);
    lv_label_set_text(lbl_hero_arrow, LV_SYMBOL_UP);
    lv_obj_set_style_text_font(lbl_hero_arrow, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_hero_arrow, LV_ALIGN_LEFT_MID, 0, 14);

    lbl_hero_s = lv_label_create(hero);
    lv_label_set_text(lbl_hero_s, "S");
    lv_obj_set_style_text_font(lbl_hero_s, &lv_font_montserrat_16, 0);
    lv_obj_set_style_opa(lbl_hero_s, LV_OPA_60, 0);
    lv_obj_align(lbl_hero_s, LV_ALIGN_RIGHT_MID, 0, 12);

    lbl_hero_v = lv_label_create(hero);
    lv_label_set_text(lbl_hero_v, "0.00");
    lv_obj_set_style_text_font(lbl_hero_v, DASH2_FONT_DELTA, 0);
    lv_obj_align_to(lbl_hero_v, lbl_hero_s, LV_ALIGN_OUT_LEFT_MID, -8, -4);

    /* ---------- SECTOR BAND ---------- */
    pill = panel(scr);
    lv_obj_set_size(pill, PILL_W, BAND_H);
    lv_obj_set_pos(pill, PAD_X, BAND_Y);
    lv_obj_set_style_radius(pill, 7, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pill, 1, 0);
    lv_obj_set_style_pad_hor(pill, 14, 0);

    lbl_pill_tag = lv_label_create(pill);
    lv_label_set_text(lbl_pill_tag, "S1");
    lv_obj_set_style_text_font(lbl_pill_tag, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(lbl_pill_tag, 2, 0);
    lv_obj_align(lbl_pill_tag, LV_ALIGN_LEFT_MID, 0, -9);

    lbl_pill_sub = lv_label_create(pill);
    lv_label_set_text(lbl_pill_sub, "VS BEST");
    lv_obj_set_style_text_font(lbl_pill_sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(lbl_pill_sub, 2, 0);
    lv_obj_set_style_opa(lbl_pill_sub, LV_OPA_60, 0);
    lv_obj_align(lbl_pill_sub, LV_ALIGN_LEFT_MID, 0, 11);

    pill_dot = panel(pill);
    lv_obj_set_size(pill_dot, 6, 6);
    lv_obj_set_style_radius(pill_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(pill_dot, LV_OPA_COVER, 0);
    lv_obj_align(pill_dot, LV_ALIGN_LEFT_MID, 62, 0);
    lv_obj_add_flag(pill_dot, LV_OBJ_FLAG_HIDDEN);
    start_blink(pill_dot, 1000);

    lbl_pill_s = lv_label_create(pill);
    lv_label_set_text(lbl_pill_s, "S");
    lv_obj_set_style_text_font(lbl_pill_s, &lv_font_montserrat_14, 0);
    lv_obj_set_style_opa(lbl_pill_s, LV_OPA_60, 0);
    lv_obj_align(lbl_pill_s, LV_ALIGN_RIGHT_MID, 0, 8);

    lbl_pill_v = lv_label_create(pill);
    lv_label_set_text(lbl_pill_v, "--.--");
    lv_obj_set_style_text_font(lbl_pill_v, DASH2_FONT_PILL, 0);
    lv_obj_align_to(lbl_pill_v, lbl_pill_s, LV_ALIGN_OUT_LEFT_MID, -5, -3);

    rail = panel(scr);
    lv_obj_set_size(rail, RAIL_W, BAND_H);
    lv_obj_set_pos(rail, PAD_X + PILL_W + BAND_GAP, BAND_Y);
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(rail, CHIP_GAP, 0);

    for (int i = 0; i < DASH2_SECTOR_COUNT; i++) {
        chip[i] = panel(rail);
        lv_obj_set_size(chip[i], 0, BAND_H);
        lv_obj_set_flex_grow(chip[i], 1);
        lv_obj_set_style_radius(chip[i], 5, 0);
        lv_obj_set_style_bg_opa(chip[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(chip[i], 1, 0);
        lv_obj_set_flex_flow(chip[i], LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(chip[i], LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(chip[i], 4, 0);

        chip_tag[i] = lv_label_create(chip[i]);
        lv_label_set_text(chip_tag[i], SECTOR_TAG[i]);
        lv_obj_set_style_text_font(chip_tag[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_letter_space(chip_tag[i], 1, 0);

        chip_val[i] = lv_label_create(chip[i]);
        lv_label_set_text(chip_val[i], "--");
        lv_obj_set_style_text_font(chip_val[i], &lv_font_montserrat_12, 0);
    }

    refresh_hero();
    refresh_band();
    apply_theme();
}

static void paint_delta_surface(lv_obj_t *box, bool faster, lv_obj_t **texts, int n)
{
    uint32_t fill = faster ? T.good_deep : T.bad_deep;
    uint32_t line = faster ? T.good      : T.bad;
    lv_obj_set_style_bg_color(box, C(fill), 0);
    lv_obj_set_style_border_color(box, C(line), 0);
    for (int i = 0; i < n; i++)
        if (texts[i]) lv_obj_set_style_text_color(texts[i], C(line), 0);
}

static void refresh_hero(void)
{
    if (!hero) return;
    bool faster = s_lap_delta < 0.0f;
    lv_label_set_text(lbl_hero_arrow, faster ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);
    lv_label_set_text_fmt(lbl_hero_v, "%.2f", fabsf(s_lap_delta));

    lv_obj_t *texts[] = { lbl_hero_cap, lbl_hero_arrow, lbl_hero_v, lbl_hero_s };
    paint_delta_surface(hero, faster, texts, 4);
    lv_obj_align_to(lbl_hero_v, lbl_hero_s, LV_ALIGN_OUT_LEFT_MID, -8, -4);
}

static void refresh_band(void)
{
    if (!pill) return;

    int idx = s_current;
    if (idx < 0) {
        idx = 0;
        for (int i = DASH2_SECTOR_COUNT - 1; i >= 0; i--) {
            if (s_sectors[i].state == DASH2_SECTOR_FASTER ||
                s_sectors[i].state == DASH2_SECTOR_SLOWER) { idx = i; break; }
        }
    }
    const sector_slot_t *cur = &s_sectors[idx];
    bool running = (cur->state == DASH2_SECTOR_ACTIVE);

    lv_label_set_text(lbl_pill_tag, SECTOR_TAG[idx]);
    lv_label_set_text(lbl_pill_sub, running ? "IN SECTOR" : "VS BEST");

    if (running) {
        lv_obj_remove_flag(pill_dot, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lbl_pill_v, s_running);
        lv_obj_add_flag(lbl_pill_s, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(pill, C(T.track), 0);
        lv_obj_set_style_border_color(pill, C(T.accent), 0);
        lv_obj_set_style_text_color(lbl_pill_tag, C(T.accent), 0);
        lv_obj_set_style_text_color(lbl_pill_sub, C(T.accent), 0);
        lv_obj_set_style_text_color(lbl_pill_v,   C(T.accent), 0);
        lv_obj_set_style_bg_color(pill_dot,       C(T.accent), 0);
    } else if (cur->state == DASH2_SECTOR_PENDING) {
        lv_obj_add_flag(pill_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(lbl_pill_s, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lbl_pill_v, "--.--");
        lv_obj_set_style_bg_color(pill, C(T.track), 0);
        lv_obj_set_style_border_color(pill, C(T.rule), 0);
        lv_obj_set_style_text_color(lbl_pill_tag, C(T.muted), 0);
        lv_obj_set_style_text_color(lbl_pill_sub, C(T.muted), 0);
        lv_obj_set_style_text_color(lbl_pill_v,   C(T.muted), 0);
    } else {
        bool faster = (cur->state == DASH2_SECTOR_FASTER);
        lv_obj_add_flag(pill_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(lbl_pill_s, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(lbl_pill_v, "%s%.2f",
                              cur->delta < 0 ? "-" : "+", fabsf(cur->delta));
        lv_obj_t *texts[] = { lbl_pill_tag, lbl_pill_sub, lbl_pill_v, lbl_pill_s };
        paint_delta_surface(pill, faster, texts, 4);
    }
    lv_obj_align_to(lbl_pill_v, lbl_pill_s, LV_ALIGN_OUT_LEFT_MID, -5, -3);

    for (int i = 0; i < DASH2_SECTOR_COUNT; i++) {
        dash2_sector_state_t st = s_sectors[i].state;
        uint32_t col;
        switch (st) {
            case DASH2_SECTOR_ACTIVE: col = T.accent; break;
            case DASH2_SECTOR_FASTER: col = T.good;   break;
            case DASH2_SECTOR_SLOWER: col = T.bad;    break;
            default:                  col = T.muted;  break;
        }
        lv_obj_set_style_bg_color(chip[i], C(T.track), 0);
        lv_obj_set_style_border_color(chip[i],
            C(st == DASH2_SECTOR_PENDING ? T.rule : col), 0);
        lv_obj_set_style_border_opa(chip[i],
            (st == DASH2_SECTOR_FASTER || st == DASH2_SECTOR_SLOWER)
                ? LV_OPA_40 : LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(chip_tag[i], C(col), 0);
        lv_obj_set_style_text_color(chip_val[i], C(col), 0);

        if (st == DASH2_SECTOR_FASTER || st == DASH2_SECTOR_SLOWER) {
            lv_label_set_text_fmt(chip_val[i], "%s%.2f",
                                  s_sectors[i].delta < 0 ? "-" : "+",
                                  fabsf(s_sectors[i].delta));
        } else {
            lv_label_set_text(chip_val[i], "--");
        }
    }
}

static void apply_theme(void)
{
    lv_obj_set_style_bg_color(scr, C(T.bg), 0);
    lv_obj_set_style_text_color(lbl_speed_cap,  C(T.muted), 0);
    lv_obj_set_style_text_color(lbl_speed_unit, C(T.muted), 0);
    lv_obj_set_style_text_color(lbl_speed_v,    C(T.fg),    0);
    lv_obj_set_style_text_color(lbl_lap_n, C(T.muted), 0);
    lv_obj_set_style_text_color(lbl_lap_v, C(T.fg),    0);
    lv_obj_set_style_text_color(lbl_best,  C(T.muted), 0);
    refresh_hero();
    refresh_band();
}

/* ============================================================================
 * Public API
 * ============================================================================ */
void ui_dash2_set_speed(int kmh)
{
    if (!lbl_speed_v) return;
    if (kmh < 0) kmh = 0;
    lv_label_set_text_fmt(lbl_speed_v, "%d", kmh);
    lv_obj_align_to(lbl_speed_unit, lbl_speed_v, LV_ALIGN_OUT_RIGHT_BOTTOM, 7, -8);
}

void ui_dash2_set_lap(int lap_num, const char *lap_time, const char *best_time)
{
    if (!lbl_lap_v) return;
    lv_label_set_text_fmt(lbl_lap_n, "LAP %d", lap_num);
    if (lap_time)  lv_label_set_text(lbl_lap_v, lap_time);
    if (best_time) lv_label_set_text_fmt(lbl_best, "BEST %s", best_time);
    lv_obj_align(box_lap, LV_ALIGN_TOP_RIGHT, -PAD_X, 40);
    lv_obj_align_to(lbl_best, box_lap, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 3);
}

void ui_dash2_set_delta(float seconds) { s_lap_delta = seconds; refresh_hero(); }

void ui_dash2_enter_sector(dash2_sector_t s)
{
    if (s < 0 || s >= DASH2_SECTOR_COUNT) return;
    if (s_current >= 0 && s_current != (int)s &&
        s_sectors[s_current].state == DASH2_SECTOR_ACTIVE) {
        s_sectors[s_current].state = DASH2_SECTOR_PENDING;
    }
    s_current = (int)s;
    s_sectors[s].state = DASH2_SECTOR_ACTIVE;
    refresh_band();
}

void ui_dash2_close_sector(dash2_sector_t s, float delta_seconds)
{
    if (s < 0 || s >= DASH2_SECTOR_COUNT) return;
    s_sectors[s].delta = delta_seconds;
    s_sectors[s].state = (delta_seconds < 0.0f) ? DASH2_SECTOR_FASTER
                                                : DASH2_SECTOR_SLOWER;
    if (s_current == (int)s) s_current = -1;
    refresh_band();
}

void ui_dash2_set_running_split(const char *split)
{
    if (!split || s_current < 0) return;
    lv_strlcpy(s_running, split, sizeof(s_running));
    if (lbl_pill_v) lv_label_set_text(lbl_pill_v, s_running);
}

void ui_dash2_reset_sectors(void)
{
    for (int i = 0; i < DASH2_SECTOR_COUNT; i++) {
        s_sectors[i].state = DASH2_SECTOR_PENDING;
        s_sectors[i].delta = 0.0f;
    }
    s_current = -1;
    refresh_band();
}

void ui_dash2_set_sector(dash2_sector_t s, dash2_sector_state_t st, float delta_seconds)
{
    if (s < 0 || s >= DASH2_SECTOR_COUNT) return;
    s_sectors[s].state = st;
    s_sectors[s].delta = delta_seconds;
    if (st == DASH2_SECTOR_ACTIVE)      s_current = (int)s;
    else if (s_current == (int)s)       s_current = -1;
    refresh_band();
}

void ui_dash2_set_mode(dash2_mode_t mode)
{
    s_mode = mode;
    T = (mode == DASH2_MODE_DAY) ? TOK_DAY : TOK_NIGHT;
    apply_theme();
}

dash2_mode_t ui_dash2_get_mode(void) { return s_mode; }

/* LOCAL: see the header. */
lv_obj_t *ui_dash2_get_speed_obj(void) { return lbl_speed_v; }
