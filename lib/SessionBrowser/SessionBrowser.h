#ifndef SESSION_BROWSER_H
#define SESSION_BROWSER_H

#include <Arduino.h>
#include "lvgl.h"
#include "EspNowProtocol.h"

/* ============================================================================
 * SessionBrowser — review a recorded session on the dash itself.
 *
 * The same three views the WiFi portal serves (summary, laps, speed trace,
 * track map), but on the device, so a run can be reviewed in the paddock
 * without a phone or laptop.
 *
 * One important difference from the browser version: laps here are computed by
 * feeding the log through the real LapManager, not a reimplementation of it.
 * The web page has to duplicate that geometry in JS and can drift from the
 * firmware; this cannot.
 *
 * Memory shape: a session is ~9,500 rows and will not fit in RAM, so the file
 * is streamed once and downsampled on the fly into fixed-size arrays (halving
 * the kept set and doubling the stride whenever they fill). Nothing scales
 * with session length, so a 40-minute log costs the same as a 5-minute one.
 * ========================================================================= */

#define SB_MAX_LAPS    64
#define SB_SPEED_PTS   240      /* speed trace resolution                    */
#define SB_TRACK_PTS   600      /* racing-line resolution                    */

class SessionBrowser {
public:
    /* Builds both screens once. Call with the display locked. */
    void begin();

    /* Populate the file list from the SD card and show it. */
    void openList();

    /* Call from loop(), with the display lock NOT held. Streaming a ~600 kB
     * session takes far longer than the 5 s loopTask watchdog, and doing it in
     * the LVGL task (where the button callback runs) holds the display lock the
     * whole time — loop() then blocks on bsp_display_lock(), stops feeding the
     * watchdog, and the board panics. So the tap only records a request and the
     * work happens here, on the task that owns the watchdog and can feed it. */
    void service();

private:
    struct Analysis {
        uint32_t rows;
        uint32_t fixRows;
        uint64_t firstEpoch, lastEpoch;
        float    maxSpeed, avgSpeed;
        double   metres;
        uint8_t  lapCount;
        uint64_t lapMs[SB_MAX_LAPS];
        uint64_t lapBestBefore[SB_MAX_LAPS];   /* best prior to that lap */
        uint64_t lapAt[SB_MAX_LAPS];           /* epoch of the crossing, for
                                                * placing marks on the x axis */
        uint64_t bestMs;

        /* Per-lap splits, read off LapManager as each lap closes. Zero when a
         * split gate was missed that lap — the lap time is still good. */
        uint64_t lapSec[SB_MAX_LAPS][3];
        uint64_t bestSec[3];        /* fastest split seen for each sector   */
        uint8_t  bestSecLap[3];     /* 1-based lap holding it, 0 = none     */
        bool     haveSectors;

        uint16_t speedN;
        uint8_t  speed[SB_SPEED_PTS];          /* km/h, clamped to 255 */

        uint16_t trackN;
        float    lat[SB_TRACK_PTS], lng[SB_TRACK_PTS];
        /* Speed sampled at the *track* stride, not the speed-trace stride —
         * the two downsample independently, so colouring the racing line needs
         * its own copy rather than an index into speed[]. */
        uint8_t  tspd[SB_TRACK_PTS];
        double   minLat, maxLat, minLng, maxLng;
    };

    static void backCb(lv_event_t *e);
    static void fileCb(lv_event_t *e);

    void showDetail(const char *path);
    bool analyse(const char *path, Analysis &a);
    void renderDetail(const char *name, const Analysis &a);

    volatile bool _pending = false;
    char          _pendingPath[80] = {0};

    lv_obj_t *_scrList   = nullptr;
    lv_obj_t *_list      = nullptr;
    lv_obj_t *_scrDetail = nullptr;
    lv_obj_t *_body      = nullptr;   /* scrollable detail content */
    lv_obj_t *_title     = nullptr;
};

extern SessionBrowser sessionBrowser;

#endif // SESSION_BROWSER_H
