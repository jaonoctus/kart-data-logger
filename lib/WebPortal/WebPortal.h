#ifndef WEB_PORTAL_H
#define WEB_PORTAL_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/* ============================================================================
 * WebPortal — on-demand SoftAP + web UI for the SD card.
 *
 * The enclosure is sealed and the SD card is not reachable, so this is the way
 * logs come off the device and configuration goes on. Connect to the AP and
 * open http://kart.local (or http://192.168.4.1):
 *
 *   GET    /                  the single-page app (embedded, no CDN — the AP
 *                             has no internet, so everything is self-contained)
 *   GET    /api/info          card size / free space / firmware info
 *   GET    /api/list?path=/   directory listing as JSON
 *   GET    /api/file?path=..  streamed download
 *   POST   /api/upload        multipart upload
 *   GET    /api/delete?path=. delete a file
 *
 * The device only serves bytes: the browser parses the CSV and draws the lap
 * table, speed trace and track map. Parsing a 9,500-row session on the ESP32
 * would be slow and memory-hungry; in JS it is instant, and richer views cost
 * the firmware nothing.
 *
 * Off by default. A SoftAP costs roughly 100 mA — comparable to the rest of the
 * board — so it is started explicitly from the dash and shown in the status bar
 * while live.
 *
 * Runs its own task: WebServer is synchronous and streaming a 600 kB log takes
 * far longer than the 5 s loopTask watchdog would tolerate.
 * ========================================================================= */

class WebPortal {
public:
    bool start();          /* bring up the AP + HTTP server (idempotent)      */
    void stop();           /* tear down and switch the radio off             */
    bool toggle();         /* returns the new running state                  */

    bool isRunning() const { return _running; }
    uint8_t clients() const;             /* stations associated with the AP  */
    const char *ssid() const { return _ssid; }
    String ip() const;

private:
    static void taskTrampoline(void *arg);
    void        task();
    void        routes();

    volatile bool _running   = false;
    volatile bool _stopWanted = false;
    TaskHandle_t  _task      = nullptr;
    char          _ssid[24]  = {0};
};

#endif // WEB_PORTAL_H
