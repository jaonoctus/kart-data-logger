#include "SessionBrowser.h"
#include <SD_MMC.h>
#include "LapManager.h"
#include "ConfigManager.h"
#include "ui.h"          /* ui_configscreen — where BACK returns to */
#include "esp_bsp.h"     /* bsp_display_lock/unlock */
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"

SessionBrowser sessionBrowser;

/* Palette kept in step with uiHelper's THEME_NIGHT. */
#define SB_BG    0x050608
#define SB_SURF  0x0D1014
#define SB_SURF2 0x14181E
#define SB_FG    0xF6F8FB
#define SB_MUT   0x6B7280
#define SB_RULE  0x1C2026
#define SB_ACC   0xFFD400
#define SB_GOOD  0x2EE07A
#define SB_BAD   0xFF3B3B

static inline lv_color_t CC(uint32_t h) { return lv_color_hex(h); }

static void fmtLap(uint64_t ms, char *out, size_t n) {
    if (ms == 0 || ms == 0xFFFFFFFFFFFFFFFFULL) { snprintf(out, n, "--"); return; }
    uint32_t m = (uint32_t)(ms / 60000);
    double   s = (ms % 60000) / 1000.0;
    if (m) snprintf(out, n, "%u:%06.3f", (unsigned)m, s);
    else   snprintf(out, n, "%.3f", s);
}

/* ---------------------------------------------------------------------------
 * Screens
 * ------------------------------------------------------------------------- */
void SessionBrowser::begin() {
    if (_scrList) return;

    /* ---- list screen ---- */
    _scrList = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scrList, CC(SB_BG), LV_PART_MAIN);
    lv_obj_remove_flag(_scrList, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_button_create(_scrList);
    lv_obj_set_size(hdr, 140, 40);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 8, 32);   /* below the status bar */
    lv_obj_set_style_bg_color(hdr, CC(SB_SURF2), LV_PART_MAIN);
    lv_obj_set_style_border_width(hdr, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(hdr, backCb, LV_EVENT_CLICKED, (void *)0);
    lv_obj_t *hl = lv_label_create(hdr);
    lv_label_set_text(hl, LV_SYMBOL_LEFT "  BACK");
    lv_obj_center(hl);
    lv_obj_set_style_text_color(hl, CC(SB_FG), LV_PART_MAIN);

    lv_obj_t *t = lv_label_create(_scrList);
    lv_label_set_text(t, "SESSIONS");
    lv_obj_align(t, LV_ALIGN_TOP_RIGHT, -12, 42);
    lv_obj_set_style_text_color(t, CC(SB_MUT), LV_PART_MAIN);

    _list = lv_list_create(_scrList);
    lv_obj_set_size(_list, 464, 236);
    lv_obj_align(_list, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_style_bg_color(_list, CC(SB_SURF), LV_PART_MAIN);
    lv_obj_set_style_border_color(_list, CC(SB_RULE), LV_PART_MAIN);

    /* ---- detail screen ---- */
    _scrDetail = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scrDetail, CC(SB_BG), LV_PART_MAIN);
    lv_obj_remove_flag(_scrDetail, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *b2 = lv_button_create(_scrDetail);
    lv_obj_set_size(b2, 140, 40);
    lv_obj_align(b2, LV_ALIGN_TOP_LEFT, 8, 32);
    lv_obj_set_style_bg_color(b2, CC(SB_SURF2), LV_PART_MAIN);
    lv_obj_set_style_border_width(b2, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(b2, backCb, LV_EVENT_CLICKED, (void *)1);
    lv_obj_t *b2l = lv_label_create(b2);
    lv_label_set_text(b2l, LV_SYMBOL_LEFT "  SESSIONS");
    lv_obj_center(b2l);
    lv_obj_set_style_text_color(b2l, CC(SB_FG), LV_PART_MAIN);

    _title = lv_label_create(_scrDetail);
    lv_label_set_text(_title, "");
    lv_obj_align(_title, LV_ALIGN_TOP_RIGHT, -12, 42);
    lv_obj_set_style_text_color(_title, CC(SB_ACC), LV_PART_MAIN);

    _body = lv_obj_create(_scrDetail);
    lv_obj_set_size(_body, 470, 236);
    lv_obj_align(_body, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_flex_flow(_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(_body, LV_DIR_VER);
    lv_obj_set_style_bg_color(_body, CC(SB_BG), LV_PART_MAIN);
    lv_obj_set_style_border_width(_body, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(_body, 10, LV_PART_MAIN);
}

void SessionBrowser::backCb(lv_event_t *e) {
    /* user_data 0 = leave the browser, 1 = detail back to the list */
    if ((intptr_t)lv_event_get_user_data(e) == 1) lv_screen_load(sessionBrowser._scrList);
    else                                          lv_screen_load(ui_configscreen);
}

void SessionBrowser::fileCb(lv_event_t *e) {
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    const char *name = lv_list_get_button_text(sessionBrowser._list, btn);
    if (!name) return;
    char path[64];
    snprintf(path, sizeof(path), "/%s", name);
    sessionBrowser.showDetail(path);
}

void SessionBrowser::openList() {
    begin();
    lv_obj_clean(_list);

    File dir = SD_MMC.open("/");
    int n = 0;
    if (dir && dir.isDirectory()) {
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            String nm = f.name();
            int sl = nm.lastIndexOf('/');
            if (sl >= 0) nm = nm.substring(sl + 1);
            bool csv = nm.length() > 4 &&
                       nm.substring(nm.length() - 4).equalsIgnoreCase(".csv");
            if (!f.isDirectory() && csv && !nm.startsWith(".")) {
                lv_obj_t *b = lv_list_add_button(_list, LV_SYMBOL_FILE, nm.c_str());
                lv_obj_set_style_text_color(b, CC(SB_FG), LV_PART_MAIN);
                lv_obj_set_style_bg_color(b, CC(SB_SURF), LV_PART_MAIN);
                lv_obj_add_event_cb(b, fileCb, LV_EVENT_CLICKED, NULL);
                n++;
            }
            f.close();
        }
        dir.close();
    }
    if (!n) {
        lv_obj_t *l = lv_label_create(_list);
        lv_label_set_text(l, "no .csv logs on the card");
        lv_obj_set_style_text_color(l, CC(SB_MUT), LV_PART_MAIN);
    }
    lv_screen_load(_scrList);
}

/* ---------------------------------------------------------------------------
 * Streaming analysis — one pass, fixed memory
 * ------------------------------------------------------------------------- */
bool SessionBrowser::analyse(const char *path, Analysis &a) {
    memset(&a, 0, sizeof(a));
    a.bestMs = 0xFFFFFFFFFFFFFFFFULL;
    a.minLat = a.minLng = 1e9;
    a.maxLat = a.maxLng = -1e9;

    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return false;
    if (f.available()) f.readStringUntil('\n');       /* header */

    /* Laps come from the real LapManager, with the finish line of whichever
     * track is selected — no second implementation to drift out of step. */
    LapManager lm;
    lm.setVerbose(false);       /* thousands of gate-trace lines otherwise */
    int sel = (int)configManager.getSelectedTrack();
    const TrackConfig *tc = configManager.getTrack(sel);
    bool haveGate = tc && tc->left_valid && tc->right_valid;
    if (haveGate) {
        FinishLine fl = { tc->left_lat, tc->left_lon, tc->right_lat, tc->right_lon };
        lm.setFinishLine(fl);
    }

    uint32_t speedStride = 1, speedSeen = 0;
    uint32_t trackStride = 1, trackSeen = 0;
    double   sumSpeed = 0;
    double   pLat = 0, pLng = 0; bool havePrev = false;

    uint32_t sinceFeed = 0;
    while (f.available()) {
        /* service() runs on loopTask, which owns the 5 s watchdog. A session is
         * thousands of rows and takes many seconds to stream, so feed it as we
         * go rather than letting the read look like a hang. */
        if (++sinceFeed >= 250) { sinceFeed = 0; esp_task_wdt_reset(); }

        String line = f.readStringUntil('\n');
        if (line.length() < 20) continue;

        unsigned long long epoch = 0;
        float sp = 0, tot = 0, gx = 0, gy = 0, st = 0;
        int sats = 0; double lat = 0, lng = 0;
        if (sscanf(line.c_str(), "%llu,%f,%f,%f,%f,%f,%d,%lf,%lf",
                   &epoch, &sp, &tot, &gx, &gy, &st, &sats, &lat, &lng) != 9) continue;

        if (a.rows == 0) a.firstEpoch = epoch;
        a.lastEpoch = epoch;
        a.rows++;
        sumSpeed += sp;
        if (sp > a.maxSpeed) a.maxSpeed = sp;

        /* Speed trace: keep every Nth; when full, halve and double the stride.
         * Bounded memory regardless of how long the session ran. */
        if (++speedSeen >= speedStride) {
            speedSeen = 0;
            if (a.speedN >= SB_SPEED_PTS) {
                for (uint16_t i = 0; i < SB_SPEED_PTS / 2; i++) a.speed[i] = a.speed[i * 2];
                a.speedN = SB_SPEED_PTS / 2;
                speedStride *= 2;
            }
            a.speed[a.speedN++] = (uint8_t)(sp > 255 ? 255 : (sp < 0 ? 0 : sp));
        }

        bool fix = sats >= 3;
        if (fix) {
            a.fixRows++;
            if (havePrev) {
                double p = 0.017453292519943295;
                double q = 0.5 - cos((lat - pLat) * p) / 2 +
                           cos(pLat * p) * cos(lat * p) * (1 - cos((lng - pLng) * p)) / 2;
                double d = 12742000 * asin(sqrt(q < 0 ? 0 : (q > 1 ? 1 : q)));
                if (d < 200) a.metres += d;          /* ignore GPS jumps */
            }
            pLat = lat; pLng = lng; havePrev = true;

            if (lat < a.minLat) a.minLat = lat;
            if (lat > a.maxLat) a.maxLat = lat;
            if (lng < a.minLng) a.minLng = lng;
            if (lng > a.maxLng) a.maxLng = lng;

            if (++trackSeen >= trackStride) {
                trackSeen = 0;
                if (a.trackN >= SB_TRACK_PTS) {
                    for (uint16_t i = 0; i < SB_TRACK_PTS / 2; i++) {
                        a.lat[i]  = a.lat[i * 2];  a.lng[i] = a.lng[i * 2];
                        a.tspd[i] = a.tspd[i * 2];
                    }
                    a.trackN = SB_TRACK_PTS / 2;
                    trackStride *= 2;
                }
                a.lat[a.trackN]  = (float)lat;
                a.lng[a.trackN]  = (float)lng;
                a.tspd[a.trackN] = (uint8_t)(sp > 255 ? 255 : (sp < 0 ? 0 : sp));
                a.trackN++;
            }

            if (haveGate && a.lapCount < SB_MAX_LAPS) {
                TelemetryMsg m = {};
                m.type = MSG_TELEMETRY; m.timestamp = epoch; m.speedKmph = sp;
                m.lat = lat; m.lng = lng; m.sats = (uint8_t)sats; m.hasFix = 1;
                if (lm.processTelemetry(m)) {
                    uint64_t lt = lm.getLastLapTime();
                    if (lt > 0) {
                        a.lapBestBefore[a.lapCount] = lm.getPreviousBestLapTime();
                        /* Sample epoch, not the interpolated crossing instant
                         * LapManager computes internally — within one sample
                         * (~55 ms at 18 Hz), which is far finer than a pixel. */
                        a.lapAt[a.lapCount] = epoch;
                        a.lapMs[a.lapCount++] = lt;
                        if (lt < a.bestMs) a.bestMs = lt;
                    }
                }
            }
        }
    }
    f.close();
    a.avgSpeed = a.rows ? (float)(sumSpeed / a.rows) : 0.0f;
    return a.rows > 1;
}

/* ---------------------------------------------------------------------------
 * Detail rendering
 * ------------------------------------------------------------------------- */

/* lv_table has no per-cell colour API — styles apply to LV_PART_ITEMS as a
 * whole. The supported way to vary one row is to intercept its draw tasks as
 * they are queued and edit the descriptor. The row is carried in id1. */
static int32_t s_bestRow = -1;      /* 1-based table row, -1 = none */

static void lapTableDrawCb(lv_event_t *e) {
    lv_draw_task_t *task = lv_event_get_draw_task(e);
    if (!task || lv_draw_task_get_type(task) != LV_DRAW_TASK_TYPE_LABEL) return;

    lv_draw_dsc_base_t *base = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(task);
    if (!base || base->part != LV_PART_ITEMS) return;
    if (s_bestRow < 0 || (int32_t)base->id1 != s_bestRow) return;

    lv_draw_label_dsc_t *lbl = (lv_draw_label_dsc_t *)base;
    lbl->color = CC(SB_GOOD);
}

static lv_obj_t *card(lv_obj_t *parent, const char *heading) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_width(c, LV_PCT(100));
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(c, CC(SB_SURF), LV_PART_MAIN);
    lv_obj_set_style_border_color(c, CC(SB_RULE), LV_PART_MAIN);
    lv_obj_set_style_border_width(c, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(c, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(c, 10, LV_PART_MAIN);
    if (heading) {
        lv_obj_t *h = lv_label_create(c);
        lv_label_set_text(h, heading);
        lv_obj_set_style_text_color(h, CC(SB_MUT), LV_PART_MAIN);
    }
    return c;
}

void SessionBrowser::renderDetail(const char *name, const Analysis &a) {
    lv_obj_clean(_body);
    lv_label_set_text(_title, name);

    char buf[220], l1[24], l2[24];
    uint32_t secs = (uint32_t)((a.lastEpoch - a.firstEpoch) / 1000);

    /* ---- summary ---- */
    lv_obj_t *c = card(_body, "SESSION");
    lv_obj_t *s = lv_label_create(c);
    fmtLap(a.bestMs, l1, sizeof(l1));
    snprintf(buf, sizeof(buf),
             "%um %02us   %.2f km\n"
             "max %.1f  avg %.1f km/h\n"
             "%lu samples (%lu with fix)\n"
             "best lap %s",
             (unsigned)(secs / 60), (unsigned)(secs % 60), a.metres / 1000.0,
             a.maxSpeed, a.avgSpeed,
             (unsigned long)a.rows, (unsigned long)a.fixRows, l1);
    lv_label_set_text(s, buf);
    lv_obj_set_style_text_color(s, CC(SB_FG), LV_PART_MAIN);

    /* ---- laps ---- */
    lv_obj_t *lc = card(_body, "LAPS");
    if (!a.lapCount) {
        lv_obj_t *l = lv_label_create(lc);
        lv_label_set_text(l, "no crossings — is the right track selected?");
        lv_obj_set_style_text_color(l, CC(SB_MUT), LV_PART_MAIN);
    } else {
        lv_obj_t *tb = lv_table_create(lc);
        lv_obj_set_width(tb, LV_PCT(100));
        lv_table_set_column_count(tb, 3);
        lv_table_set_row_count(tb, a.lapCount + 1);
        lv_table_set_cell_value(tb, 0, 0, "LAP");
        lv_table_set_cell_value(tb, 0, 1, "TIME");
        lv_table_set_cell_value(tb, 0, 2, "VS BEST");
        s_bestRow = -1;
        for (uint8_t i = 0; i < a.lapCount; i++) {
            snprintf(buf, sizeof(buf), "%u", (unsigned)(i + 1));
            lv_table_set_cell_value(tb, i + 1, 0, buf);
            fmtLap(a.lapMs[i], l1, sizeof(l1));
            bool best = (a.lapMs[i] == a.bestMs);
            /* First match wins: if two laps tie on the millisecond, only the
             * earlier one is painted rather than both. */
            if (best && s_bestRow < 0) s_bestRow = i + 1;
            snprintf(buf, sizeof(buf), "%s%s", l1, best ? " *" : "");
            lv_table_set_cell_value(tb, i + 1, 1, buf);
            /* Delta against the best as it stood before this lap — same rule
             * the live dashboard uses. */
            uint64_t pb = a.lapBestBefore[i];
            if (pb == 0 || pb == 0xFFFFFFFFFFFFFFFFULL) {
                lv_table_set_cell_value(tb, i + 1, 2, "--");
            } else {
                double d = ((double)(int64_t)(a.lapMs[i] - pb)) / 1000.0;
                snprintf(l2, sizeof(l2), "%+.3f", d);
                lv_table_set_cell_value(tb, i + 1, 2, l2);
            }
        }
        lv_obj_set_style_bg_color(tb, CC(SB_SURF), LV_PART_ITEMS);
        lv_obj_set_style_text_color(tb, CC(SB_FG), LV_PART_ITEMS);
        lv_obj_set_style_border_color(tb, CC(SB_RULE), LV_PART_ITEMS);

        /* Opt in to per-draw-task events so the best lap's row can be tinted. */
        lv_obj_add_flag(tb, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
        lv_obj_add_event_cb(tb, lapTableDrawCb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    }

    /* ---- speed trace ----
     * Drawn on a canvas rather than lv_chart: the chart widget has no way to
     * annotate arbitrary lines, and the useful part here is exactly those
     * annotations — where each lap ended, and where max and average sit. */
    if (a.speedN > 1) {
        lv_obj_t *sc = card(_body, "SPEED");
        const int CW = 430, CH = 150;
        const int padL = 36, padR = 8, padT = 12, padB = 18;
        const int x0 = padL, x1 = CW - padR, y0 = padT, y1 = CH - padB;

        static uint8_t *sbuf = nullptr;
        if (!sbuf) {
            sbuf = (uint8_t *)heap_caps_malloc(
                LV_CANVAS_BUF_SIZE(CW, CH, 16, LV_DRAW_BUF_ALIGN), MALLOC_CAP_SPIRAM);
        }

        if (sbuf) {
            lv_obj_t *cv = lv_canvas_create(sc);
            lv_canvas_set_buffer(cv, sbuf, CW, CH, LV_COLOR_FORMAT_RGB565);
            lv_obj_set_size(cv, CW, CH);
            lv_canvas_fill_bg(cv, CC(SB_SURF2), LV_OPA_COVER);

            float top = a.maxSpeed > 10.0f ? a.maxSpeed * 1.12f : 10.0f;
            auto yOf = [&](float v) -> int32_t {
                float f = v / top; if (f < 0) f = 0; if (f > 1) f = 1;
                return (int32_t)(y1 - f * (y1 - y0));
            };

            lv_layer_t layer;
            lv_canvas_init_layer(cv, &layer);

            lv_draw_line_dsc_t d;
            lv_draw_line_dsc_init(&d);
            d.opa = LV_OPA_COVER;

            lv_draw_label_dsc_t t;
            lv_draw_label_dsc_init(&t);
            t.font = &lv_font_montserrat_14;

            /* Text passed to lv_draw_* is read when the layer is flushed, not
             * at call time, so these must outlive the loop below. */
            static char sMax[24], sAvg[24], sLap[SB_MAX_LAPS][6];

            /* --- lap boundaries, vertical dashed, numbered along the x axis --- */
            uint64_t span = (a.lastEpoch > a.firstEpoch) ? (a.lastEpoch - a.firstEpoch) : 1;
            d.width = 1; d.dash_width = 3; d.dash_gap = 3;
            d.color = CC(SB_RULE);
            for (uint8_t i = 0; i < a.lapCount; i++) {
                double fx = (double)(a.lapAt[i] - a.firstEpoch) / (double)span;
                int32_t x = (int32_t)(x0 + fx * (x1 - x0));
                d.p1.x = x; d.p1.y = y0; d.p2.x = x; d.p2.y = y1;
                lv_draw_line(&layer, &d);

                snprintf(sLap[i], sizeof(sLap[i]), "%u", (unsigned)(i + 1));
                t.color = CC(SB_MUT);
                t.text  = sLap[i];
                lv_area_t la = { (int32_t)(x - 8), (int32_t)(y1 + 2),
                                 (int32_t)(x + 8), (int32_t)(CH) };
                lv_draw_label(&layer, &t, &la);
            }

            /* --- average, dashed --- */
            d.color = CC(SB_MUT);
            int32_t ya = yOf(a.avgSpeed);
            d.p1.x = x0; d.p1.y = ya; d.p2.x = x1; d.p2.y = ya;
            lv_draw_line(&layer, &d);
            snprintf(sAvg, sizeof(sAvg), "avg %.0f", a.avgSpeed);
            t.color = CC(SB_MUT); t.text = sAvg;
            lv_area_t aa = { 0, (int32_t)(ya - 8), (int32_t)(x0 - 3), (int32_t)(ya + 9) };
            lv_draw_label(&layer, &t, &aa);

            /* --- max, dashed --- */
            d.color = CC(SB_BAD);
            int32_t ym = yOf(a.maxSpeed);
            d.p1.x = x0; d.p1.y = ym; d.p2.x = x1; d.p2.y = ym;
            lv_draw_line(&layer, &d);
            snprintf(sMax, sizeof(sMax), "max %.0f", a.maxSpeed);
            t.color = CC(SB_BAD); t.text = sMax;
            lv_area_t ma = { 0, (int32_t)(ym - 8), (int32_t)(x0 - 3), (int32_t)(ym + 9) };
            lv_draw_label(&layer, &t, &ma);

            /* --- the trace itself, solid, on top --- */
            d.dash_width = 0; d.dash_gap = 0;
            d.width = 2; d.color = CC(SB_ACC);
            d.round_start = 1; d.round_end = 1;
            for (uint16_t i = 1; i < a.speedN; i++) {
                d.p1.x = x0 + (int32_t)((i - 1) * (int32_t)(x1 - x0) / (a.speedN - 1));
                d.p1.y = yOf(a.speed[i - 1]);
                d.p2.x = x0 + (int32_t)(i * (int32_t)(x1 - x0) / (a.speedN - 1));
                d.p2.y = yOf(a.speed[i]);
                lv_draw_line(&layer, &d);
            }

            lv_canvas_finish_layer(cv, &layer);
        }
    }

    /* ---- track map ---- */
    if (a.trackN > 2 && a.maxLat > a.minLat) {
        lv_obj_t *mc = card(_body, "TRACK");
        lv_obj_t *holder = lv_obj_create(mc);
        const int MW = 430, MH = 200;
        lv_obj_set_size(holder, MW, MH);
        lv_obj_remove_flag(holder, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(holder, CC(SB_SURF2), LV_PART_MAIN);
        lv_obj_set_style_border_width(holder, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(holder, 0, LV_PART_MAIN);

        /* Longitude degrees shrink with latitude, so scale x by cos(lat) or a
         * circuit comes out stretched sideways. */
        double kx = cos(((a.minLat + a.maxLat) / 2.0) * 0.017453292519943295);
        double spanX = (a.maxLng - a.minLng) * kx; if (spanX <= 0) spanX = 1e-9;
        double spanY = (a.maxLat - a.minLat);      if (spanY <= 0) spanY = 1e-9;
        double sc = fmin((MW - 12) / spanX, (MH - 12) / spanY);
        double ox = (MW - spanX * sc) / 2.0, oy = (MH - spanY * sc) / 2.0;

        static lv_point_precise_t pts[SB_TRACK_PTS];
        for (uint16_t i = 0; i < a.trackN; i++) {
            pts[i].x = (lv_value_precise_t)(ox + (a.lng[i] - a.minLng) * kx * sc);
            pts[i].y = (lv_value_precise_t)(MH - (oy + (a.lat[i] - a.minLat) * sc));
        }

        /* Colour by speed. lv_line takes a single colour for the whole polyline,
         * and one widget per segment would mean hundreds of objects, so draw
         * into a canvas instead: one object, per-segment colour. The buffer is
         * ~170 kB, far too much for internal DRAM but nothing against 8 MB of
         * PSRAM, and it is allocated once and reused. */
        static uint8_t *cbuf = nullptr;
        if (!cbuf) {
            cbuf = (uint8_t *)heap_caps_malloc(
                LV_CANVAS_BUF_SIZE(MW, MH, 16, LV_DRAW_BUF_ALIGN), MALLOC_CAP_SPIRAM);
        }

        if (cbuf) {
            lv_obj_t *cv = lv_canvas_create(holder);
            lv_canvas_set_buffer(cv, cbuf, MW, MH, LV_COLOR_FORMAT_RGB565);
            lv_obj_set_size(cv, MW, MH);
            lv_obj_center(cv);
            lv_canvas_fill_bg(cv, CC(SB_SURF2), LV_OPA_COVER);

            float top = a.maxSpeed > 1.0f ? a.maxSpeed : 1.0f;

            lv_layer_t layer;
            lv_canvas_init_layer(cv, &layer);
            lv_draw_line_dsc_t d;
            lv_draw_line_dsc_init(&d);
            d.width       = 3;
            d.opa         = LV_OPA_COVER;
            d.round_start = 1;
            d.round_end   = 1;

            for (uint16_t i = 1; i < a.trackN; i++) {
                float f = a.tspd[i] / top;
                if (f < 0) f = 0; if (f > 1) f = 1;
                /* Blue (slow) through to red (flat out), brightening with pace. */
                d.color = lv_color_hsv_to_rgb((uint16_t)((1.0f - f) * 210.0f), 85,
                                              (uint8_t)(55 + f * 45));
                d.p1 = pts[i - 1];
                d.p2 = pts[i];
                lv_draw_line(&layer, &d);
            }
            lv_canvas_finish_layer(cv, &layer);

            lv_obj_t *key = lv_label_create(mc);
            lv_label_set_text_fmt(key, "slow " LV_SYMBOL_RIGHT " fast   (0 - %.0f km/h)",
                                  a.maxSpeed);
            lv_obj_set_style_text_color(key, CC(SB_MUT), LV_PART_MAIN);
        } else {
            /* No PSRAM for the canvas — fall back to a flat polyline. */
            lv_obj_t *ln = lv_line_create(holder);
            lv_line_set_points(ln, pts, a.trackN);
            lv_obj_set_style_line_color(ln, CC(SB_GOOD), LV_PART_MAIN);
            lv_obj_set_style_line_width(ln, 2, LV_PART_MAIN);
            lv_obj_set_style_line_rounded(ln, true, LV_PART_MAIN);
        }
    }
}

/* Runs in LVGL context (button callback): record the request and get out.
 * Everything expensive happens in service(), on loopTask. */
void SessionBrowser::showDetail(const char *path) {
    lv_obj_clean(_body);
    lv_label_set_text(_title, "reading…");
    lv_obj_t *l = lv_label_create(_body);
    lv_label_set_text(l, "streaming the log from the card…");
    lv_obj_set_style_text_color(l, CC(SB_MUT), LV_PART_MAIN);
    lv_screen_load(_scrDetail);

    strncpy(_pendingPath, path, sizeof(_pendingPath) - 1);
    _pendingPath[sizeof(_pendingPath) - 1] = '\0';
    _pending = true;
}

void SessionBrowser::service() {
    if (!_pending) return;
    _pending = false;

    /* PSRAM, not .bss: internal DRAM is the scarce pool WiFi and BLE compete
     * for, and this is pure CPU-accessed data. */
    static Analysis *ap = nullptr;
    if (!ap) ap = (Analysis *)heap_caps_malloc(sizeof(Analysis), MALLOC_CAP_SPIRAM);
    if (!ap) {
        log_e("SessionBrowser: no PSRAM for analysis buffer (%u bytes)",
              (unsigned)sizeof(Analysis));
        return;
    }
    Analysis &a = *ap;

    const char *name = strrchr(_pendingPath, '/');
    name = name ? name + 1 : _pendingPath;

    uint32_t t0 = millis();
    bool ok = analyse(_pendingPath, a);   /* feeds the watchdog as it goes */
    log_i("SessionBrowser: %s -> %s in %lu ms (%lu rows, %u laps)",
          name, ok ? "ok" : "FAILED", (unsigned long)(millis() - t0),
          (unsigned long)a.rows, (unsigned)a.lapCount);

    /* Take the lock only for the drawing, which is fast. */
    bsp_display_lock(0);
    if (ok) {
        renderDetail(name, a);
    } else {
        lv_obj_clean(_body);
        lv_label_set_text(_title, name);
        lv_obj_t *l = lv_label_create(_body);
        lv_label_set_text(l, "could not read this log");
        lv_obj_set_style_text_color(l, CC(SB_BAD), LV_PART_MAIN);
    }
    bsp_display_unlock();
}
