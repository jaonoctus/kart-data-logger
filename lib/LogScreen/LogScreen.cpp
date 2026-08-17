#include "LogScreen.h"
#include "LogBuffer.h"

LogScreen logScreen;

/* Deliberately smaller than the ring. LVGL lays a wrapped label out in one
 * pass, so handing it the full 64 KB would stall the display for the sake of
 * history nobody scrolls back through on a 3.5" panel. render() fills this with
 * the NEWEST lines that fit (~120); the ring and /dash.log keep the rest. */
#define LS_TEXT_BYTES ((size_t)12 * 1024)

/* Matches the palette the rest of the dash uses (THEME_NIGHT in uiHelper.h),
 * copied rather than included so this screen does not pull in the whole
 * UiHelper/LapManager/BSP header chain for four colours. */
static const uint32_t LS_BG      = 0x050608;
static const uint32_t LS_SURFACE = 0x14181E;
static const uint32_t LS_FG      = 0xF6F8FB;
static const uint32_t LS_MUTED   = 0x6B7280;
static const uint32_t LS_BAD     = 0xFF3B3B;

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             lv_event_cb_t cb, uint32_t colour) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 110, 44);
    lv_obj_set_style_radius(btn, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(LS_SURFACE), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(btn, lv_color_hex(colour), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(btn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_ext_click_area(btn, 6);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    lv_obj_set_style_text_color(lbl, lv_color_hex(colour), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN | LV_STATE_DEFAULT);

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
    lv_obj_set_style_bg_color(_screen, lv_color_hex(LS_BG), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(_screen, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(_screen, 8, LV_PART_MAIN | LV_STATE_DEFAULT);

    _title = lv_label_create(_screen);
    lv_obj_align(_title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(_title, "SYSTEM LOG");
    lv_obj_set_style_text_color(_title, lv_color_hex(LS_FG), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(_title, &lv_font_montserrat_18, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Scrollable text region. Newest lines are at the bottom, and open() scrolls
     * there — when something has just failed, the last line is the one wanted. */
    /* Height in real pixels. LV_PCT() returns an ENCODED coordinate, so
     * arithmetic on it (LV_PCT(100) - 92) yields nonsense, not "percent minus
     * 92px" — that collapsed this box to a single line. */
    const int32_t screen_h = lv_display_get_vertical_resolution(NULL);
    lv_obj_t *box = lv_obj_create(_screen);
    lv_obj_set_size(box, LV_PCT(100), screen_h - 100);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_color(box, lv_color_hex(LS_SURFACE), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(box, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(box, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(box, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(box, 6, LV_PART_MAIN | LV_STATE_DEFAULT);

    _body = lv_label_create(box);
    lv_obj_set_width(_body, LV_PCT(100));
    lv_label_set_long_mode(_body, LV_LABEL_LONG_WRAP);
    lv_label_set_text(_body, "");
    lv_obj_set_style_text_color(_body, lv_color_hex(LS_FG), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(_body, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_align(make_button(_screen, "BACK",  backCb,  LS_FG),
                 LV_ALIGN_BOTTOM_LEFT,  0, 0);
    lv_obj_align(make_button(_screen, "CLEAR", clearCb, LS_BAD),
                 LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

void LogScreen::refresh() {
    if (!_body || !_text) return;

    logBuffer.render(_text, LS_TEXT_BYTES);
    lv_label_set_text(_body, _text[0] ? _text : "(nothing logged yet)");

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
