#include "uiHelper.h"

/* Status bar uses an amber 'warn' tone in addition to good/bad. */
#define DASH_WARN_HEX 0xFFB020

static dash_theme_t T = THEME_NIGHT;
static UiHelper *s_instance = nullptr;

// Static member definitions
const char   *UiHelper::s_track_names[SETUP_MAX_TRACKS] = {};
int           UiHelper::s_track_count = 0;
int           UiHelper::s_track_idx   = 0;
bool          UiHelper::s_dirty       = false;
setup_coord_t UiHelper::s_line_l      = {};
setup_coord_t UiHelper::s_line_r      = {};
lv_obj_t     *UiHelper::s_cam_tag     = nullptr;
lv_obj_t     *UiHelper::s_cam_var     = nullptr;
bool          UiHelper::s_cam_linked  = false;
bool          UiHelper::s_cam_recording = false;
bool          UiHelper::s_cam_gpslock = false;
uint8_t       UiHelper::s_cam_batt    = 255;
lv_obj_t     *UiHelper::s_wifi_panel   = nullptr;
lv_obj_t     *UiHelper::s_wifi_var     = nullptr;
lv_obj_t     *UiHelper::s_wifi_btn_lbl = nullptr;
bool          UiHelper::s_wifi_running = false;
uint8_t       UiHelper::s_wifi_clients = 0;
char          UiHelper::s_wifi_ip[20]  = {0};

/* helper */
static inline lv_color_t C(uint32_t hex) { return lv_color_hex(hex); }

// C bridge called from lib/ui/ui_theme.cpp (0=dark, 1=light)
extern "C" void ui_helper_set_theme(int mode) {
    if (s_instance) s_instance->setTheme(mode == 0 ? DASH_MODE_NIGHT : DASH_MODE_DAY);
}

void UiHelper::init() {
    s_instance = this;
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = {
            .task_priority     = 4,
            .task_stack        = 16384,
            .task_affinity     = -1,
            .task_max_sleep_ms = 500,
            .timer_period_ms   = 5,
        },
        .rotate = LV_DISPLAY_ROTATION_270,
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    bsp_display_lock(0);

    // Initialize the UI
    ui_init();

    //Reparent the status bar panel to the persistent top layer
    lv_obj_set_parent(ui_panelstatus, lv_layer_top());
    // Delete the now-empty Screen_TopBar to save RAM
    lv_obj_delete(ui_statusbarscreen);

    // GoPro cell — added here rather than in SquareLine so the generated UI
    // stays untouched. The status bar is a SPACE_BETWEEN flex row, so this
    // lands between the DISP and GPS cells on its own.
    build_camera_cell();
    build_wifi_cell();
    build_wifi_button();

    // Set the theme as saved
    setTheme(DASH_MODE_NIGHT);
    setSpeed(0);
    setGx(0);
    setGy(0);
    setDelta(0, true);
    setLap(0, "", "");

    // Track setup initialization
    s_track_count = 0;
    s_track_idx   = 0;
    s_dirty       = false;
    s_line_l      = { 0, 0, false };
    s_line_r      = { 0, 0, false };

    refresh_track_name();
    refresh_coord_row(SETUP_LINE_L);
    refresh_coord_row(SETUP_LINE_R);
    refresh_dirty();

    bsp_display_unlock();
}

/* ============================================================================
 * UPDATE
 * ============================================================================ */
void UiHelper::setSpeed(int kmh) {
    if (!ui_labelspeedvar) return;
    lv_label_set_text_fmt(ui_labelspeedvar, "%d", kmh);
}

void UiHelper::setGx(float gx) {
    if (!ui_bargx) return;
    int v = (int)(gx * 100.0f);
    if (v >  300) v =  300;
    if (v < -300) v = -300;
    lv_bar_set_value(ui_bargx, v, LV_ANIM_ON);
    lv_label_set_text_fmt(ui_labelgxvar, "G-X %.2f", gx);
}

void UiHelper::setGy(float gy) {
    if (!ui_bargy) return;
    /* invert sign so that acceleration (negative gy) fills above center,
     * matching the web prototype. */
    int v = (int)(-gy * 100.0f);
    if (v >  200) v =  200;
    if (v < -200) v = -200;
    lv_bar_set_value(ui_bargy, v, LV_ANIM_ON);
    lv_label_set_text_fmt(ui_labelgyvar, "%.2f G-Y", gy);
}

void UiHelper::setLap(uint8_t lap_num, const char *lap_str, const char *best_str) {
    if (ui_labellapnum) lv_label_set_text_fmt(ui_labellapnum, "LAP %d", lap_num);
    if (ui_labellapvar && lap_str)  lv_label_set_text(ui_labellapvar, lap_str);
    if (ui_labellapbest  && best_str) lv_label_set_text_fmt(ui_labellapbest, "BEST %s", best_str);
}

void UiHelper::setDelta(float seconds, bool faster) {
    if (!ui_paneldelta) return;
    uint32_t bg = faster ? T.good_deep : T.bad_deep;
    uint32_t fg = faster ? T.good      : T.bad;
    lv_obj_set_style_bg_color    (ui_paneldelta,       C(bg), 0);
    lv_obj_set_style_border_color(ui_paneldelta,       C(fg), 0);
    lv_obj_set_style_text_color  (ui_labelarrow, C(fg), 0);
    lv_obj_set_style_text_color  (ui_labeldeltavar,     C(fg), 0);
    lv_obj_set_style_text_color  (ui_labeldeltas,     C(fg), 0);
    lv_label_set_text(ui_labelarrow, faster ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);
    lv_label_set_text_fmt(ui_labeldeltavar, "%.2f", fabsf(seconds));
}

void UiHelper::setDisplay(uint8_t pct) {
    if (!ui_labelvardisplay) return;

    if (pct == 255)
        lv_label_set_text(ui_labelvardisplay, "--");
    else {
        uint32_t hc = batt_color(pct);
        lv_label_set_text_fmt(ui_labelvardisplay, "%d%%", pct);
        lv_obj_set_style_text_color(ui_labelvardisplay, C(hc), 0);
    }
}

void UiHelper::setGps(uint8_t pct) {
    if (!ui_labelvargps) return;

    if (pct == 255)
        lv_label_set_text(ui_labelvargps, "--");
    else {
        uint32_t hc = gps_color(pct);
        lv_label_set_text_fmt(ui_labelvargps, "%d", pct);
        lv_obj_set_style_text_color(ui_labelvargps, C(hc), 0);
    }
}

/* ============================================================================
 * GOPRO STATUS CELL
 * Mirrors the SquareLine DISP / GPS cells so the bar stays visually uniform.
 * Four pieces of state, one glance:
 *   text   "--" when the camera is not linked, otherwise its battery %
 *   colour red while recording, battery-graded otherwise
 *   tag    green "CAM" once the camera itself has a GPS lock
 * ============================================================================ */
void UiHelper::build_camera_cell(void) {
    if (!ui_panelstatus || s_cam_var) return;

    lv_obj_t *panel = lv_obj_create(ui_panelstatus);
    lv_obj_set_width (panel, LV_SIZE_CONTENT);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_align (panel, LV_ALIGN_CENTER);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(panel, C(T.bg), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(panel, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(panel, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

    s_cam_tag = lv_label_create(panel);
    lv_obj_set_width (s_cam_tag, LV_SIZE_CONTENT);
    lv_obj_set_height(s_cam_tag, LV_SIZE_CONTENT);
    lv_obj_set_align (s_cam_tag, LV_ALIGN_CENTER);
    lv_label_set_text(s_cam_tag, "CAM");
    lv_obj_set_style_text_letter_space(s_cam_tag, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_cam_tag, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);

    s_cam_var = lv_label_create(panel);
    lv_obj_set_width (s_cam_var, LV_SIZE_CONTENT);
    lv_obj_set_height(s_cam_var, LV_SIZE_CONTENT);
    lv_obj_set_align (s_cam_var, LV_ALIGN_CENTER);
    lv_label_set_text(s_cam_var, "--");
    lv_obj_set_style_text_font(s_cam_var, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Sit between DISP and GPS rather than after both. */
    lv_obj_move_to_index(panel, 1);

    refresh_camera();
}

void UiHelper::refresh_camera(void) {
    if (!s_cam_var || !s_cam_tag) return;

    lv_obj_set_style_text_color(s_cam_tag,
        C(s_cam_linked && s_cam_gpslock ? T.good : T.muted), 0);

    if (!s_cam_linked) {
        lv_label_set_text(s_cam_var, "--");
        lv_obj_set_style_text_color(s_cam_var, C(T.muted), 0);
        return;
    }

    if (s_cam_batt == 255)
        lv_label_set_text(s_cam_var, s_cam_recording ? "REC" : "??");
    else if (s_cam_recording)
        lv_label_set_text_fmt(s_cam_var, "REC %d%%", s_cam_batt);
    else
        lv_label_set_text_fmt(s_cam_var, "%d%%", s_cam_batt);

    lv_obj_set_style_text_color(s_cam_var,
        C(s_cam_recording ? T.bad : batt_color(s_cam_batt)), 0);
}

void UiHelper::setCamera(bool linked, bool recording, uint8_t battPct, bool gpsLock) {
    if (linked == s_cam_linked && recording == s_cam_recording &&
        battPct == s_cam_batt && gpsLock == s_cam_gpslock) return;   /* no-op */

    s_cam_linked    = linked;
    s_cam_recording = recording;
    s_cam_batt      = battPct;
    s_cam_gpslock   = gpsLock;
    refresh_camera();
}

/* ============================================================================
 * WIFI PORTAL — status cell + config-screen toggle
 * Both built at runtime so the SquareLine project stays untouched. The cell is
 * hidden entirely while the portal is off: a SoftAP costs ~100 mA, so its
 * presence on the bar should mean something.
 * ============================================================================ */
extern "C" void ui_helper_toggle_wifi(void);   /* implemented in main_display */
extern "C" void ui_helper_open_sessions(void); /* implemented in main_display */

static void wifi_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) ui_helper_toggle_wifi();
}
static void sessions_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) ui_helper_open_sessions();
}

void UiHelper::build_wifi_cell(void) {
    if (!ui_panelstatus || s_wifi_var) return;

    s_wifi_panel = lv_obj_create(ui_panelstatus);
    lv_obj_set_width (s_wifi_panel, LV_SIZE_CONTENT);
    lv_obj_set_height(s_wifi_panel, LV_SIZE_CONTENT);
    lv_obj_set_align (s_wifi_panel, LV_ALIGN_CENTER);
    lv_obj_set_flex_flow(s_wifi_panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_wifi_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_wifi_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_wifi_panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_wifi_panel, C(T.bg), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_wifi_panel, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_wifi_panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(s_wifi_panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(s_wifi_panel, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

    s_wifi_var = lv_label_create(s_wifi_panel);
    lv_obj_set_width (s_wifi_var, LV_SIZE_CONTENT);
    lv_obj_set_height(s_wifi_var, LV_SIZE_CONTENT);
    lv_obj_set_align (s_wifi_var, LV_ALIGN_CENTER);
    lv_label_set_text(s_wifi_var, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(s_wifi_var, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_move_to_index(s_wifi_panel, 2);
    refresh_wifi();
}

/* Shared styling for the runtime-added config-screen buttons, so they are
 * indistinguishable from the SquareLine ones. */
lv_obj_t *UiHelper::make_setup_button(const char *text, lv_event_cb_t cb,
                                      lv_obj_t **out_label) {
    lv_obj_t *btn = lv_button_create(ui_panelsetupbuttons);
    lv_obj_set_width (btn, 250);
    lv_obj_set_height(btn, 50);
    lv_obj_set_align (btn, LV_ALIGN_TOP_MID);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_ext_click_area(btn, 5);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(btn, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, C(T.surface2), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(btn, C(T.muted), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(btn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    /* Fixed height + no shrink: inside a scrolling flex column the buttons
     * would otherwise be squeezed to fit rather than overflow into a scroll. */
    lv_obj_set_style_flex_grow(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_width (lbl, LV_SIZE_CONTENT);
    lv_obj_set_height(lbl, LV_SIZE_CONTENT);
    lv_obj_set_align (lbl, LV_ALIGN_CENTER);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, C(T.fg), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    if (out_label) *out_label = lbl;
    return btn;
}

void UiHelper::build_wifi_button(void) {
    if (!ui_panelsetupbuttons || s_wifi_btn_lbl) return;

    /* The panel was built non-scrollable for its original two buttons. With
     * SESSIONS and WIFI PORTAL added it overflows, so let it scroll rather
     * than pushing the last button off the bottom where it cannot be reached. */
    lv_obj_add_flag(ui_panelsetupbuttons, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(ui_panelsetupbuttons, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui_panelsetupbuttons, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_align(ui_panelsetupbuttons, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_bottom(ui_panelsetupbuttons, 12, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* SESSIONS sits above the portal button — reviewing a run is the more
     * common reason to come here than moving files off the card. */
    make_setup_button("SESSIONS", sessions_btn_cb, nullptr);
    make_setup_button("WIFI PORTAL: OFF", wifi_btn_cb, &s_wifi_btn_lbl);
    refresh_wifi();
}

void UiHelper::refresh_wifi(void) {
    if (s_wifi_panel) {
        if (s_wifi_running) lv_obj_remove_flag(s_wifi_panel, LV_OBJ_FLAG_HIDDEN);
        else                lv_obj_add_flag(s_wifi_panel, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_wifi_var) {
        /* A connected client is the interesting state — it means someone is
         * actually pulling files, so don't cut the radio. */
        if (s_wifi_clients > 0)
            lv_label_set_text_fmt(s_wifi_var, LV_SYMBOL_WIFI " %d", s_wifi_clients);
        else
            lv_label_set_text(s_wifi_var, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(s_wifi_var,
            C(s_wifi_clients > 0 ? T.good : T.accent), 0);
    }
    if (s_wifi_btn_lbl) {
        if (s_wifi_running && s_wifi_ip[0])
            lv_label_set_text_fmt(s_wifi_btn_lbl, "WIFI PORTAL: %s", s_wifi_ip);
        else
            lv_label_set_text(s_wifi_btn_lbl, s_wifi_running ? "WIFI PORTAL: ON" : "WIFI PORTAL: OFF");
        lv_obj_set_style_text_color(s_wifi_btn_lbl, C(s_wifi_running ? T.accent : T.fg), 0);
    }
}

/* A portal that refuses to start must say so — silently staying on OFF looks
 * exactly like a dead button, which is how this went unnoticed the first time. */
void UiHelper::setWifiError(void) {
    s_wifi_running = false;
    s_wifi_clients = 0;
    s_wifi_ip[0]   = '\0';
    refresh_wifi();
    if (s_wifi_btn_lbl) {
        lv_label_set_text(s_wifi_btn_lbl, "WIFI PORTAL: FAILED");
        lv_obj_set_style_text_color(s_wifi_btn_lbl, C(T.bad), 0);
    }
}

void UiHelper::setWifi(bool running, uint8_t clients, const char *ip) {
    bool same = (running == s_wifi_running) && (clients == s_wifi_clients) &&
                (ip ? strncmp(ip, s_wifi_ip, sizeof(s_wifi_ip)) == 0 : s_wifi_ip[0] == '\0');
    if (same) return;

    s_wifi_running = running;
    s_wifi_clients = clients;
    if (ip) { strncpy(s_wifi_ip, ip, sizeof(s_wifi_ip) - 1); s_wifi_ip[sizeof(s_wifi_ip) - 1] = '\0'; }
    else    { s_wifi_ip[0] = '\0'; }
    refresh_wifi();
}

/* ============================================================================
 * TRACK SETUP
 * ============================================================================ */

void UiHelper::setTracks(const char *const *names, int count) {
    if (count < 0) count = 0;
    if (count > SETUP_MAX_TRACKS) count = SETUP_MAX_TRACKS;
    s_track_count = count;
    for (int i = 0; i < count; i++) s_track_names[i] = names[i];
    if (s_track_idx >= s_track_count) s_track_idx = 0;
    refresh_track_name();
}

void UiHelper::setTrackIdx(int idx) {
    if (s_track_count == 0) return;
    if (idx < 0) idx = 0;
    if (idx >= s_track_count) idx = s_track_count - 1;
    s_track_idx = idx;
    refresh_track_name();
}

void UiHelper::setStartL(double lat, double lon, bool valid) {
    s_line_l.lat = lat; s_line_l.lon = lon; s_line_l.valid = valid;
    refresh_coord_row(SETUP_LINE_L);
}

void UiHelper::setStartR(double lat, double lon, bool valid) {
    s_line_r.lat = lat; s_line_r.lon = lon; s_line_r.valid = valid;
    refresh_coord_row(SETUP_LINE_R);
}

void UiHelper::setDirty(bool dirty) {
    s_dirty = dirty;
    refresh_dirty();
}

/* ============================================================================
 * THEME
 * Status bar and dashboard are always dark
 * ============================================================================ */
void UiHelper::setTheme(dash_mode_t mode) {
    T = (mode == DASH_MODE_DAY) ? THEME_DAY : THEME_NIGHT;

    /* ----- Runtime-built cells (GoPro, WiFi portal) ----- */
    refresh_camera();
    refresh_wifi();

    /* ----- Config ----- */
    lv_obj_set_style_bg_color(ui_configscreen,      C(T.bg), LV_PART_MAIN);

    // Header
    lv_obj_set_style_bg_color    (ui_panelsetup,        C(T.bg),        LV_PART_MAIN);
    lv_obj_set_style_border_color(ui_panelsetup,        C(T.rule),      0);
    lv_obj_set_style_bg_color    (ui_panelsetupback,    C(T.bg),        LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color    (ui_panelsetupback,    C(T.surface2),  LV_PART_MAIN| LV_STATE_PRESSED);
    lv_obj_set_style_text_color  (ui_labelback,         C(T.fg2),       LV_PART_MAIN);
    lv_obj_set_style_text_color  (ui_labelbacktext,     C(T.fg2),       LV_PART_MAIN);
    lv_obj_set_style_text_color  (ui_labelsetup,        C(T.fg),        LV_PART_MAIN);
    // Buttons
    lv_obj_set_style_bg_color  (ui_panelsetupbuttons,   C(T.bg),        LV_PART_MAIN);
    lv_obj_set_style_bg_color  (ui_buttontracksetup,    C(T.surface2),  LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_labeltracksetuptext, C(T.fg),        LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_labeltracksetup,     C(T.fg),        LV_PART_MAIN);
    lv_obj_set_style_bg_color  (ui_buttonstartsession,  C(T.surface2),  LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_labelstartsession,   C(T.fg),        LV_PART_MAIN);
    lv_obj_set_style_bg_color  (ui_paneldarklight,      C(T.bg),        LV_PART_MAIN);
    // Theme Selector
    if (ui_buttondark) {
        bool night = (mode == DASH_MODE_NIGHT);
        lv_obj_t *active_btn   = night ? ui_buttondark            : ui_buttondark1;
        lv_obj_t *active_lbl   = night ? ui_labeltracksetuptext2  : ui_labeltracksetuptext1;
        lv_obj_t *inactive_btn = night ? ui_buttondark1           : ui_buttondark;
        lv_obj_t *inactive_lbl = night ? ui_labeltracksetuptext1  : ui_labeltracksetuptext2;
        lv_obj_set_style_bg_color  (active_btn,   C(T.accent),      LV_PART_MAIN);
        lv_obj_set_style_text_color(active_lbl,   C(T.fg),          LV_PART_MAIN);
        lv_obj_set_style_bg_color  (inactive_btn, C(T.surface2),    LV_PART_MAIN);
        lv_obj_set_style_text_color(inactive_lbl, C(T.fg),          LV_PART_MAIN);
    }

    /* ----- Track setup ----- */
    lv_obj_set_style_bg_color(ui_trackscreen, C(T.bg), LV_PART_MAIN);
    // Header
    lv_obj_set_style_bg_color    (ui_paneltracksetup,       C(T.bg),        LV_PART_MAIN);
    lv_obj_set_style_border_color(ui_paneltracksetup,       C(T.rule),      0);
    lv_obj_set_style_bg_color    (ui_paneltracksetupback,   C(T.bg),        LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color    (ui_paneltracksetupback,   C(T.surface2),  LV_PART_MAIN| LV_STATE_PRESSED);
    lv_obj_set_style_text_color  (ui_labeltrackback,        C(T.fg2),       LV_PART_MAIN);
    lv_obj_set_style_text_color  (ui_labeltrackbacktext,    C(T.fg2),       LV_PART_MAIN);
    lv_obj_set_style_text_color  (ui_labeltracksetup1,      C(T.fg),        LV_PART_MAIN);
    lv_obj_set_style_bg_color    (ui_paneldirty,            C(T.bg),        LV_PART_MAIN);
    lv_obj_set_style_bg_color    (ui_dirtydot,              C(T.accent),    LV_PART_MAIN);
    lv_obj_set_style_shadow_color(ui_dirtydot,              C(T.accent),    LV_PART_MAIN );
    lv_obj_set_style_text_color  (ui_labeldirtytext,        C(T.muted),     LV_PART_MAIN);
    // Body
    lv_obj_set_style_bg_color(ui_panelbody, C(T.bg), LV_PART_MAIN);
    // Track Selector
    lv_obj_set_style_bg_color    (ui_paneltrack,            C(T.bg),        LV_PART_MAIN);
    lv_obj_set_style_bg_color    (ui_paneltrackheader,      C(T.bg),        LV_PART_MAIN);
    lv_obj_set_style_text_color  (ui_labeltrackheader,      C(T.muted),     LV_PART_MAIN);
    lv_obj_set_style_text_color  (ui_labeltrackpos,         C(T.muted),     LV_PART_MAIN);
    lv_obj_set_style_bg_color    (ui_paneltrackstepper,     C(T.bg),        LV_PART_MAIN);
    lv_obj_set_style_bg_color    (ui_panelstepper,          C(T.surface),   LV_PART_MAIN);
    lv_obj_set_style_border_color(ui_panelstepper,          C(T.rule),      0);
    lv_obj_set_flex_grow         (ui_panelstepper,          1);
    lv_obj_set_style_bg_color    (ui_buttonstepperl,        C(T.surface2),  LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color    (ui_buttonstepperl,        C(T.muted),     LV_PART_MAIN| LV_STATE_PRESSED);
    lv_obj_set_style_border_color(ui_buttonstepperl,        C(T.rule),      0);
    lv_obj_set_style_text_color  (ui_labelstepperleft,      C(T.fg),        LV_PART_MAIN);
    lv_obj_set_style_text_color  (ui_labelsteppertrackname, C(T.fg),        LV_PART_MAIN);
    lv_obj_set_flex_grow         (ui_labelsteppertrackname, 1);
    lv_obj_set_style_bg_color    (ui_buttonstepperr,        C(T.surface2),  LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color    (ui_buttonstepperr,        C(T.muted),     LV_PART_MAIN| LV_STATE_PRESSED);
    lv_obj_set_style_border_color(ui_buttonstepperr,        C(T.rule),      0);
    lv_obj_set_style_text_color  (ui_labelstepperright,     C(T.fg),        LV_PART_MAIN);
    lv_obj_set_style_bg_color    (ui_buttonaddtrack,        C(T.surface2),  LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color    (ui_buttonaddtrack,        C(T.muted),     LV_PART_MAIN| LV_STATE_PRESSED);
    lv_obj_set_style_border_color(ui_buttonaddtrack,        C(T.rule),      0);
    lv_obj_set_style_text_color  (ui_labeladdtrack,         C(T.fg),        LV_PART_MAIN);

    // Coordinates
    lv_obj_set_style_bg_color  (ui_panelcoord,          C(T.bg),    LV_PART_MAIN);
    lv_obj_set_style_bg_color  (ui_panelstartline,      C(T.bg),    LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_labelstartline,      C(T.muted), LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_labelstartlinehint,  C(T.muted), LV_PART_MAIN);
    // Left Point
    lv_obj_set_style_bg_color    (ui_panellinel,    C(T.surface),   LV_PART_MAIN);
    lv_obj_set_style_border_color(ui_panellinel,    C(T.rule),      0);
    lv_obj_set_style_text_color  (ui_labeltagl,     C(T.accent),    LV_PART_MAIN);
    lv_obj_set_style_text_color  (ui_labellatl,     C(T.fg),        LV_PART_MAIN);
    lv_obj_set_flex_grow         (ui_labellatl,     1);
    lv_obj_set_style_text_color  (ui_labellonl,     C(T.fg),        LV_PART_MAIN);
    lv_obj_set_flex_grow         (ui_labellonl,     1);
    lv_obj_set_style_text_color  (ui_labelemptyl,   C(T.muted),     LV_PART_MAIN);
    lv_obj_set_flex_grow         (ui_labelemptyl,   2);
    lv_obj_set_style_bg_color    (ui_buttonpinl,    C(T.surface2),  LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color    (ui_buttonpinl,    C(T.accent),    LV_PART_MAIN| LV_STATE_PRESSED);
    lv_obj_set_style_border_color(ui_buttonpinl,    C(T.rule),      0);
    lv_obj_set_style_text_color  (ui_labelpinl,     C(T.fg),        LV_PART_MAIN);
    // Right Point
    lv_obj_set_style_bg_color    (ui_panelliner,    C(T.surface),   LV_PART_MAIN);
    lv_obj_set_style_border_color(ui_panelliner,    C(T.rule),      0);
    lv_obj_set_style_text_color  (ui_labeltagr,     C(T.accent),    LV_PART_MAIN);
    lv_obj_set_style_text_color  (ui_labellatr,     C(T.fg),        LV_PART_MAIN);
    lv_obj_set_flex_grow         (ui_labellatr,     1);
    lv_obj_set_style_text_color  (ui_labellonr,     C(T.fg),        LV_PART_MAIN);
    lv_obj_set_flex_grow         (ui_labellonr,     1);
    lv_obj_set_style_text_color  (ui_labelemptyr,   C(T.muted),     LV_PART_MAIN);
    lv_obj_set_flex_grow         (ui_labelemptyr,   2);
    lv_obj_set_style_bg_color    (ui_buttonpinr,    C(T.surface2),  LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color    (ui_buttonpinr,    C(T.accent),    LV_PART_MAIN| LV_STATE_PRESSED);
    lv_obj_set_style_border_color(ui_buttonpinr,    C(T.rule),      0);
    lv_obj_set_style_text_color  (ui_labelpinr,     C(T.fg),        LV_PART_MAIN);
    // Action Bar
    lv_obj_set_style_bg_color    (ui_panelactionbar,    C(T.bg),    LV_PART_MAIN);
    lv_obj_set_style_border_color(ui_panelactionbar,    C(T.rule),  0);
    // Cancel
    lv_obj_set_style_bg_color  (ui_buttoncancel,    C(T.surface),   LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color  (ui_buttoncancel,    C(T.surface2),  LV_PART_MAIN| LV_STATE_PRESSED);
    lv_obj_set_flex_grow       (ui_buttoncancel,    1);
    lv_obj_set_style_text_color(ui_labelcancel,     C(T.fg2),        LV_PART_MAIN);
    // Save
    lv_obj_set_style_bg_color  (ui_buttonsave,      C(T.accent),    LV_PART_MAIN| LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color  (ui_buttonsave,      C(T.surface2),  LV_PART_MAIN| LV_STATE_PRESSED);
    lv_obj_set_style_text_color(ui_buttonsave,      C(T.accent_fg), LV_PART_MAIN);
    lv_obj_set_flex_grow       (ui_buttonsave,      14);
    lv_obj_set_flex_grow       (ui_buttoncancel,    10);
}

/* ============================================================================
 * SESSION STATE
 * ============================================================================ */
void UiHelper::setSessionState(bool active) {
    if (ui_labelstartsession)
        lv_label_set_text(ui_labelstartsession, active ? "STOP SESSION" : "START SESSION");

    if (ui_panelrecording) {
        if (active)
            lv_obj_remove_flag(ui_panelrecording, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(ui_panelrecording, LV_OBJ_FLAG_HIDDEN);
    }
}

void UiHelper::tickRecordingPanel() {
    if (!ui_panelrecording || lv_obj_has_flag(ui_panelrecording, LV_OBJ_FLAG_HIDDEN)) return;
    uint32_t c = (millis() / 500) % 2 ? T.bad : T.bad_deep;
    lv_obj_set_style_bg_color(ui_panelrecording, C(c), LV_PART_MAIN);
}

/* ============================================================================
 * STATUS BAR SETTERS
 * ============================================================================ */
uint32_t UiHelper::batt_color(uint8_t pct) {
    if (pct >= 50) return T.good;
    if (pct >= 20 || (millis() / 500) % 2) return DASH_WARN_HEX;
    return T.bad;
}
uint32_t UiHelper::gps_color(uint8_t n) {
    if (n >= 8) return T.good;
    if (n >= 4 || (millis() / 500) % 2) return DASH_WARN_HEX;
    return T.bad;
}

/* ============================================================================
 * TRACK SETUP - REFRESHERS
 * ============================================================================ */
void UiHelper::refresh_track_name(void) {
    if (!ui_labelsteppertrackname) return;
    if (s_track_count == 0) {
        lv_label_set_text(ui_labelsteppertrackname, "NO TRACKS");
        lv_label_set_text(ui_labeltrackpos, "0 / 0");
        return;
    }
    lv_label_set_text(ui_labelsteppertrackname, s_track_names[s_track_idx]);
    lv_label_set_text_fmt(ui_labeltrackpos, "%d / %d", s_track_idx + 1, s_track_count);
}

void UiHelper::refresh_coord_row(setup_line_side_t side) {
    setup_coord_t *c    = (side == SETUP_LINE_L) ? &s_line_l : &s_line_r;
    lv_obj_t *lat       = (side == SETUP_LINE_L) ? ui_labellatl   : ui_labellatr;
    lv_obj_t *lon       = (side == SETUP_LINE_L) ? ui_labellonl   : ui_labellonr;
    lv_obj_t *empty     = (side == SETUP_LINE_L) ? ui_labelemptyl : ui_labelemptyr;
    lv_obj_t *pin_lbl   = (side == SETUP_LINE_L) ? ui_labelpinl   : ui_labelpinr;
    if (!lat) return;

    if (c->valid) {
        char buf[24];
        snprintf(buf, sizeof(buf), "LAT %.4f%c",
                 fabs(c->lat), c->lat >= 0 ? 'N' : 'S');
        lv_label_set_text(lat, buf);
        snprintf(buf, sizeof(buf), "LON %.4f%c",
                 fabs(c->lon), c->lon >= 0 ? 'E' : 'W');
        lv_label_set_text(lon, buf);
        lv_obj_clear_flag(lat,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lon,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag  (empty, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(pin_lbl, "RESET");
    } else {
        lv_obj_add_flag  (lat,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag  (lon,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(empty, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(pin_lbl, "PIN");
    }
}

void UiHelper::refresh_dirty(void) {
    if (!ui_dirtydot) return;
    if (s_dirty) {
        lv_obj_clear_flag(ui_dirtydot, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(ui_labeldirtytext, "UNSAVED");
        lv_obj_set_style_text_color(ui_labeldirtytext, C(T.accent), 0);
    } else {
        lv_obj_add_flag(ui_dirtydot, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(ui_labeldirtytext, "SAVED");
        lv_obj_set_style_text_color(ui_labeldirtytext, C(T.muted), 0);
    }
}

/* ============================================================================
 * TRACK SETUP — C BRIDGES (called from ui_theme.cpp)
 * ============================================================================ */
extern "C" int  ui_helper_get_track_idx()      { return s_instance ? s_instance->getTrackIdx() : 0; }
extern "C" void ui_helper_set_track_idx(int i) { if (s_instance) s_instance->setTrackIdx(i); }
extern "C" void ui_helper_set_dirty(bool d)    { if (s_instance) s_instance->setDirty(d); }

extern "C" void ui_helper_set_start_l(double lat, double lon, bool valid) {
    if (s_instance) s_instance->setStartL(lat, lon, valid);
}
extern "C" void ui_helper_set_start_r(double lat, double lon, bool valid) {
    if (s_instance) s_instance->setStartR(lat, lon, valid);
}
extern "C" void ui_helper_set_track_names(const char *const *names, int n) {
    if (s_instance) s_instance->setTracks(names, n);
}