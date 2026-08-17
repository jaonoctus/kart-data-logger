/*
 * DisplayGFX — parallel Arduino_GFX display backend for board JC3248W535N.
 *
 * Reimplements the BSP-compatible public API (esp_bsp.h) on top of the
 * Arduino_GFX library using a full-frame Arduino_Canvas framebuffer (PSRAM).
 *
 * Hypothesis being A/B tested: the existing esp_lcd backend wedges on a
 * QSPI polling-write spin; Arduino_GFX's full-canvas single-blit path
 * (gfx->flush()) should avoid it.
 *
 * Wiring/resolution/rotation mirrors the proven reference for this board:
 *   https://github.com/byte-me404/JC3248W535_lvgl_test
 *
 * Build: env display_gfx (DisplayBSP is lib_ignored so these headers win).
 */

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_bsp.h"

// ── Hardware literals (JC3248W535N) ──────────────────────────────────────────
// QSPI panel
#define TFT_CS        45
#define TFT_SCK       47
#define TFT_SDA0      21
#define TFT_SDA1      48
#define TFT_SDA2      40
#define TFT_SDA3      39
#define TFT_BL        1          // backlight, active HIGH
#define QSPI_CLOCK    40000000UL

// Native panel resolution; the UI runs rotated => landscape 480x320.
#define TFT_RES_W     320
#define TFT_RES_H     480
// Arduino_Canvas rotation index: 1 == 90°, 3 == 270°. The enclosure mounts the
// panel upside down relative to the original build, hence 270 rather than 90.
// gfx_touch_read_cb maps touch to match — keep the two in step.
#define TFT_ROT       3
#define LV_HOR_RES    480        // rotated horizontal resolution (LVGL space)
#define LV_VER_RES    320        // rotated vertical resolution (LVGL space)

// Touch (AXS15231B) over I2C / Arduino Wire
#define TOUCH_ADDR    0x3B
#define TOUCH_SDA     4
#define TOUCH_SCL     8
#define TOUCH_FREQ    400000

// ── Arduino_GFX objects (constructed at startup) ─────────────────────────────
static Arduino_DataBus *s_bus    = nullptr;
static Arduino_GFX     *s_panel  = nullptr;  // raw AXS15231B panel (rotation 0, native 320x480)
static Arduino_Canvas  *s_gfx    = nullptr;  // full-frame framebuffer (PSRAM), 90° in software

// ── LVGL plumbing ────────────────────────────────────────────────────────────
static lv_display_t    *s_disp   = nullptr;
static SemaphoreHandle_t s_lvgl_mux = nullptr;

// QSPI flush telemetry consumed by the shared src/main_display.cpp.
// The Arduino_GFX path does not have a polling-write spin to measure, so these
// stay at zero — kept (with C linkage) only so main_display.cpp links.
extern "C" {
    volatile uint32_t lvgl_port_frame_drops = 0;
    volatile uint32_t lvgl_port_max_stall_us = 0;
}

// ── LVGL flush callback ──────────────────────────────────────────────────────
// Copy the rendered region into the software-rotated canvas; blit the full
// framebuffer to the panel once on the last fragment of the refresh. (This QSPI
// panel can't do hardware-rotated partial windows, so a full-frame blit is used.)
static void gfx_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint32_t w = lv_area_get_width(area);
    uint32_t h = lv_area_get_height(area);

    // Undo LVGL's LV_COLOR_16_SWAP=1 (esp_lcd backend needs it) so Arduino_GFX gets
    // native-endian RGB565 — otherwise it double-swaps (grey->magenta).
    lv_draw_sw_rgb565_swap(px_map, w * h);

    s_gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);

    if (lv_display_flush_is_last(disp)) {
        s_gfx->flush();  // full-frame canvas blit
    }
    lv_display_flush_ready(disp);
}

// ── Touch read ───────────────────────────────────────────────────────────────
// Proven command sequence for the AXS15231B touch controller at addr 0x3B.
// Returns native coordinates (nx in 0..319, ny in 0..479) then maps to the
// rotation-90 LVGL space (480x320).
#define TOUCH_MAX_POINTS 1
static void gfx_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    // Match the proven esp_lcd driver exactly: 11-byte read command (length 0x08
    // in bytes 6/7), then read TOUCH_MAX_POINTS*6+2 = 8 bytes.
    static const uint8_t cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00,
                                    (TOUCH_MAX_POINTS * 6 + 2) >> 8,
                                    (TOUCH_MAX_POINTS * 6 + 2) & 0xff, 0x00, 0x00, 0x00};

    Wire.beginTransmission(TOUCH_ADDR);
    Wire.write(cmd, sizeof(cmd));
    uint8_t err = Wire.endTransmission();        // 0 == ACKed

    uint8_t d[8] = {0};
    int got = Wire.requestFrom((int)TOUCH_ADDR, (int)sizeof(d));
    int n = 0;
    while (n < (int)sizeof(d) && Wire.available()) d[n++] = Wire.read();

    uint8_t  num = d[1];
    uint16_t nx  = ((d[2] & 0x0F) << 8) | d[3];   // native X, 0..319
    uint16_t ny  = ((d[4] & 0x0F) << 8) | d[5];   // native Y, 0..479

    // Valid press: bus ACKed, full read, finger count in 1..MAX, coords in range.
    bool valid = (err == 0) && (got == (int)sizeof(d)) &&
                 (num >= 1) && (num <= TOUCH_MAX_POINTS) &&
                 (nx <= (TFT_RES_W - 1)) && (ny <= (TFT_RES_H - 1));

    if (valid) {
        // Map native -> LVGL space (480x320). Must track TFT_ROT above: the 270°
        // case is the 90° mapping with both axes flipped.
#if TFT_ROT == 3
        int32_t lx = (TFT_RES_H - 1) - ny;
        int32_t ly = nx;
#elif TFT_ROT == 1
        int32_t lx = ny;
        int32_t ly = (TFT_RES_W - 1) - nx;
#else
#error "TFT_ROT must be 1 (90 deg) or 3 (270 deg) - portrait needs new LV_*_RES too"
#endif
        if (lx < 0) lx = 0; else if (lx > LV_HOR_RES - 1) lx = LV_HOR_RES - 1;
        if (ly < 0) ly = 0; else if (ly > LV_VER_RES - 1) ly = LV_VER_RES - 1;

        data->point.x = lx;
        data->point.y = ly;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ── LVGL service task ────────────────────────────────────────────────────────
// Keeps the shared main_display.cpp unchanged (it never pumps LVGL itself).
static void gfx_lvgl_task(void *arg) {
    (void)arg;
    for (;;) {
        if (bsp_display_lock(0)) {
            lv_timer_handler();
            bsp_display_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ── Public BSP-compatible API ────────────────────────────────────────────────
extern "C" lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *cfg) {
    // 1. Build the Arduino_GFX stack (mirrors the reference exactly).
    s_bus   = new Arduino_ESP32QSPI(TFT_CS, TFT_SCK, TFT_SDA0, TFT_SDA1, TFT_SDA2, TFT_SDA3);
    // Panel stays at rotation 0 (native 320x480); the Canvas does the 90° rotation in
    // software and blits the full native framebuffer. (Direct hardware-rotated partial
    // writes collapse to a 2px strip on this QSPI panel — MADCTL MV path is unusable
    // for partial windows here, so the full-canvas blit is required.)
    s_panel = new Arduino_AXS15231B(s_bus, GFX_NOT_DEFINED, 0 /*rot*/, false /*IPS*/,
                                    TFT_RES_W, TFT_RES_H);
    s_gfx   = new Arduino_Canvas(TFT_RES_W, TFT_RES_H, s_panel, 0, 0, TFT_ROT);
    if (!s_gfx->begin(QSPI_CLOCK)) {
        Serial.println("[DisplayGFX] gfx begin() failed");
        return nullptr;
    }
    s_gfx->fillScreen(0x0000 /* RGB565 black */);

    // 2. Touch I2C bus.
    Wire.begin(TOUCH_SDA, TOUCH_SCL, TOUCH_FREQ);

    // 3. LVGL core init + tick.
    lv_init();
    lv_tick_set_cb((lv_tick_get_cb_t)millis);

    // 4. Display at the rotated (landscape) resolution the UI uses.
    s_disp = lv_display_create(LV_HOR_RES, LV_VER_RES);
    lv_display_set_flush_cb(s_disp, gfx_flush_cb);

    // 5. Two partial draw buffers (RGB565 = 2 bytes/px). Prefer INTERNAL RAM
    // (LVGL renders into these every frame; PSRAM is much slower for that).
    // Fall back to PSRAM only if internal is exhausted.
    //
    // GFX_DRAW_LINES is a memory/latency knob, and cheap in this backend: the
    // flush callback only copies into the full-frame canvas and blits once, on
    // the last fragment. Halving the buffers doubles the memcpy count but not
    // the QSPI traffic, so the cost is small — while 40 lines held 76.8 kB of
    // internal DRAM, which is the same scarce pool WiFi must allocate from and
    // cannot take from PSRAM. At 16 lines that drops to ~30 kB, leaving room
    // for the SoftAP to come up.
#ifndef GFX_DRAW_LINES
#define GFX_DRAW_LINES 16
#endif
    const uint32_t buf_px    = (uint32_t)LV_HOR_RES * GFX_DRAW_LINES;
    const uint32_t buf_bytes = buf_px * 2;
    void *b1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    void *b2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!b1) b1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    if (!b2) b2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    if (!b1 || !b2) {
        Serial.println("[DisplayGFX] draw buffer alloc failed");
        return nullptr;
    }
    /* Both internal is the intended outcome, so it is log_i — warning on the
     * healthy path just trains you to ignore the alert banner. A buffer that
     * fell back to PSRAM is the real event: LVGL renders into these every frame
     * and PSRAM is markedly slower, so it earns a warning. */
    const bool b1_int = esp_ptr_internal(b1);
    const bool b2_int = esp_ptr_internal(b2);
    if (b1_int && b2_int) {
        log_i("[GFX] draw bufs: both internal (%u bytes each)", (unsigned)buf_bytes);
    } else {
        log_w("[GFX] draw bufs FELL BACK TO PSRAM: b1=%s b2=%s (%u bytes each) — "
              "internal DRAM exhausted, expect slower rendering",
              b1_int ? "internal" : "psram",
              b2_int ? "internal" : "psram", (unsigned)buf_bytes);
    }
    lv_display_set_buffers(s_disp, b1, b2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    // 6. Touch input device.
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, gfx_touch_read_cb);

    // 7. Recursive LVGL mutex for bsp_display_lock/unlock.
    s_lvgl_mux = xSemaphoreCreateRecursiveMutex();

    // 8. LVGL service task, mirroring the old cfg.
    int prio  = cfg ? cfg->lvgl_port_cfg.task_priority : 4;
    int stack = cfg ? cfg->lvgl_port_cfg.task_stack    : 16384;
    int core  = (cfg && cfg->lvgl_port_cfg.task_affinity >= 0)
                    ? cfg->lvgl_port_cfg.task_affinity : tskNO_AFFINITY;
    xTaskCreatePinnedToCore(gfx_lvgl_task, "lvgl_gfx", stack, nullptr, prio, nullptr, core);

    return s_disp;
}

extern "C" void bsp_display_backlight_on(void) {
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
}

extern "C" void bsp_display_backlight_off(void) {
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, LOW);
}

extern "C" bool bsp_display_lock(uint32_t timeout_ms) {
    if (!s_lvgl_mux) return true;  // not started yet
    TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lvgl_mux, ticks) == pdTRUE;
}

extern "C" void bsp_display_unlock(void) {
    if (s_lvgl_mux) xSemaphoreGiveRecursive(s_lvgl_mux);
}
