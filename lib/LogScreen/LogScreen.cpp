#include "LogScreen.h"
#include "LogBuffer.h"
#include "dash_theme.h"

LogScreen logScreen;

/* Deliberately smaller than the ring. LVGL lays a wrapped label out in one
 * pass, so handing it the full 64 KB would stall the display for the sake of
 * history nobody scrolls back through on a 3.5" panel. render() fills this with
 * the NEWEST lines that fit (~120); the ring and /dash.log keep the rest. */
#define LS_TEXT_BYTES ((size_t)12 * 1024)

/* Layout. LS_STATUS_BAR_H is ui_panelstatus's height (480x26, reparented to
 * lv_layer_top() in UiHelper::init(), so it floats over this screen too);
 * everything else is derived from it so the text box always ends exactly one
 * pad above the buttons whatever the panel height turns out to be. */
#define LS_PAD           8
#define LS_STATUS_BAR_H  26
#define LS_TITLE_H       24     /* montserrat_18 line box */
#define LS_BTN_H         44
#define LS_BTN_W        110

/* Selector 0 == LV_PART_MAIN | LV_STATE_DEFAULT, and is what the hand-written
 * UI code in this repo passes. Spelling it out on every call buried the one
 * argument that actually differs between them. */

/* The dash palette, from the shared leaf header. Pinned to night like the other
 * hand-built review screens: the live theme lives in a uiHelper.cpp static that
 * is not exported, and this screen is read in a pit lane, not in daylight. */
static const dash_theme_t &TH = THEME_NIGHT;
static inline lv_color_t C(uint32_t hex) { return lv_color_hex(hex); }

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             lv_event_cb_t cb, uint32_t colour) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, LS_BTN_W, LS_BTN_H);
    lv_obj_set_style_radius(btn, 3, 0);
    lv_obj_set_style_bg_color(btn, C(TH.surface2), 0);
    lv_obj_set_style_bg_opa(btn, 255, 0);
    lv_obj_set_style_border_color(btn, C(colour), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_opa(btn, 255, 0);
    lv_obj_set_ext_click_area(btn, 6);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    lv_obj_set_style_text_color(lbl, C(colour), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

void LogScreen::begin() {
    if (_screen) return;

    _text = (char *)ps_malloc(LS_TEXT_BYTES);
    if (!_text) _text = (char *)malloc(LS_TEXT_BYTES);
    if (_text) _text[0] = '\0';

    _screen = lv_obj_create(NULL);
    lv_obj_remove_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(_screen, C(TH.bg), 0);
    lv_obj_set_style_bg_opa(_screen, 255, 0);
    lv_obj_set_style_pad_all(_screen, LS_PAD, 0);

    /* Below the status bar, which UiHelper reparents to lv_layer_top() and so
     * floats over every screen — at y=0 the title rendered behind it. Same
     * offset SessionBrowser uses for the same reason. */
    _title = lv_label_create(_screen);
    lv_obj_align(_title, LV_ALIGN_TOP_LEFT, 0, LS_STATUS_BAR_H - LS_PAD);
    lv_label_set_text(_title, "SYSTEM LOG");
    lv_obj_set_style_text_color(_title, C(TH.fg), 0);
    lv_obj_set_style_text_font(_title, &lv_font_montserrat_18, 0);

    /* Scrollable text region. Newest lines are at the bottom, and open() scrolls
     * there — when something has just failed, the last line is the one wanted. */
    /* Height in real pixels. LV_PCT() returns an ENCODED coordinate, so
     * arithmetic on it (LV_PCT(100) - 92) yields nonsense, not "percent minus
     * 92px" — that collapsed this box to a single line. */
    const int32_t screen_h = lv_display_get_vertical_resolution(NULL);
    const int32_t box_y     = LS_STATUS_BAR_H - LS_PAD + LS_TITLE_H;
    lv_obj_t *box = lv_obj_create(_screen);
    lv_obj_set_size(box, LV_PCT(100), screen_h - 2 * LS_PAD - box_y - LS_BTN_H - LS_PAD);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, box_y);
    lv_obj_set_style_bg_color(box, C(TH.surface2), 0);
    lv_obj_set_style_bg_opa(box, 255, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 3, 0);
    lv_obj_set_style_pad_all(box, 6, 0);

    _body = lv_label_create(box);
    lv_obj_set_width(_body, LV_PCT(100));
    lv_label_set_long_mode(_body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(_body, "");
    lv_obj_set_style_text_color(_body, C(TH.fg), 0);
    lv_obj_set_style_text_font(_body, &lv_font_montserrat_14, 0);

    lv_obj_align(make_button(_screen, "BACK",  backCb,  TH.fg),
                 LV_ALIGN_BOTTOM_LEFT,  0, 0);
    lv_obj_align(make_button(_screen, "CLEAR", clearCb, TH.bad),
                 LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

void LogScreen::refresh() {
    if (!_body || !_text) return;

    logBuffer.render(_text, LS_TEXT_BYTES);
    /* _static: _text is ours and outlives the label, so LVGL can point at it
     * instead of mallocing and copying another 12 KB on every refresh. */
    lv_label_set_text_static(_body, _text[0] ? _text : "(nothing logged yet)");

    lv_label_set_text_fmt(_title, "SYSTEM LOG  %u err  %u warn",
                          (unsigned)logBuffer.errorCount(),
                          (unsigned)logBuffer.warningCount());

    /* Jump to the newest line. */
    lv_obj_scroll_to_y(lv_obj_get_parent(_body), LV_COORD_MAX, LV_ANIM_OFF);
}

void LogScreen::open() {
    if (!_screen) begin();
    if (!_screen) return;

    lv_obj_t *cur = lv_screen_active();
    if (cur != _screen) _prev = cur;

    refresh();
    lv_screen_load(_screen);

    /* Opening the log is the acknowledgement — the banner should not still be
     * shouting about errors the user is currently reading. */
    logBuffer.acknowledge();
}

void LogScreen::backCb(lv_event_t *e) {
    (void)e;
    if (logScreen._prev) lv_screen_load(logScreen._prev);
}

void LogScreen::clearCb(lv_event_t *e) {
    (void)e;
    logBuffer.clear();
    logScreen.refresh();
}
