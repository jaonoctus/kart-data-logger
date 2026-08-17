#ifndef LOG_SCREEN_H
#define LOG_SCREEN_H

#include <Arduino.h>
#include "lvgl.h"

/* ============================================================================
 * LogScreen — read the captured log on the dash itself.
 *
 * Reached two ways: the SYSTEM LOG button on the config screen, and a tap on
 * the alert banner that appears over the dashboard when something has gone
 * wrong. Both land here.
 *
 * The text comes from LogBuffer's RAM ring, not the SD file, so opening it is
 * a memcpy and can safely happen inside the LVGL callback. Reading the file
 * back would be slow enough to need SessionBrowser's deferred-service dance;
 * the ring covers "what just went wrong", which is what this screen is for.
 * ========================================================================= */

class LogScreen {
public:
    /* Builds the screen once. Call with the display locked. */
    void begin();

    /* Shows it, remembering the caller's screen so BACK returns there. */
    void open();

private:
    static void backCb(lv_event_t *e);
    static void clearCb(lv_event_t *e);

    void refresh();

    lv_obj_t *_screen = nullptr;
    lv_obj_t *_body   = nullptr;   /* the scrollable label holding the text */
    lv_obj_t *_title  = nullptr;
    lv_obj_t *_prev   = nullptr;   /* screen to return to on BACK */
    char     *_text   = nullptr;   /* render target, PSRAM */
};

extern LogScreen logScreen;

#endif // LOG_SCREEN_H
