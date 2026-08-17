#ifndef LOG_BUFFER_H
#define LOG_BUFFER_H

#include <Arduino.h>

/* ============================================================================
 * LogBuffer — capture every log line the firmware emits, on the device.
 *
 * The dash is sealed and runs with nothing attached to USB, so `log_e()` output
 * goes nowhere and a failure at the track leaves only "GPS 0" on screen with no
 * account of what happened. This keeps the last LOGBUF_LINES lines in RAM for
 * the log screen and appends them to the SD card so they survive a power cycle.
 *
 * It hooks esp_log_set_vprintf() rather than wrapping LOG_ERROR, which means it
 * captures the Arduino core, LVGL, NimBLE and the ESP-IDF too — every call site
 * in the binary, present and future, with no edits to any of them.
 *
 * Ordering: begin() must run FIRST in setup(), before the SD card is mounted.
 * Lines logged before the mount stay queued in the ring and are flushed once
 * service() finds the card, so the GPS handshake still reaches the file.
 * ========================================================================= */

/* 512 lines ~= 20 minutes at the steady-state rate (a battery line and a GPS
 * line every 5s), which is the length of a session worth reviewing. At 128
 * bytes a line that is 64 KB, taken from 8 MB of PSRAM. */
#define LOGBUF_LINES     512
#define LOGBUF_LINE_LEN  128    /* longer lines are truncated, never dropped     */
#define LOGBUF_MAX_BYTES (512UL * 1024UL)   /* rotate the file past this */

class LogBuffer {
public:
    /* Installs the vprintf hook and allocates the ring (PSRAM for preference —
     * internal DRAM is scarce on this board). Safe to call once. */
    void begin();

    /* Flushes queued lines to SD. Call from loop(), NOT from an LVGL callback:
     * SD writes are slow and the LVGL task holds the display lock. */
    void service();

    uint16_t errorCount()   const { return _errors; }
    uint16_t warningCount() const { return _warnings; }
    uint16_t problemCount() const { return _errors + _warnings; }

    /* Marks everything currently in the log as seen, which is what clears the
     * dashboard alert. Does not erase the ring or the file. */
    void acknowledge();

    /* Copies the ring into `out`, oldest line first, NUL-terminated.
     * Returns bytes written. Safe to call from an LVGL callback. */
    size_t render(char *out, size_t outLen);

    /* Empties the ring and deletes the SD log. */
    void clear();

private:
    static int  vprintfHook(const char *fmt, va_list args);
    static void putcHook(char c);
    void push(const char *line);

    char    *_ring     = nullptr;   /* LOGBUF_LINES * LOGBUF_LINE_LEN */
    uint16_t _head     = 0;         /* next slot to write             */
    uint16_t _count    = 0;         /* lines held, <= LOGBUF_LINES    */
    uint32_t _written  = 0;         /* total lines ever pushed        */
    uint32_t _flushed  = 0;         /* total lines written to SD      */
    uint16_t _errors   = 0;
    uint16_t _warnings = 0;
    bool     _started  = false;
};

extern LogBuffer logBuffer;

#endif // LOG_BUFFER_H
