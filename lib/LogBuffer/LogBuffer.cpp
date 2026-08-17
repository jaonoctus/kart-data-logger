#include "LogBuffer.h"
#include <esp_log.h>
#include <esp_rom_sys.h>

#if defined(JC3248W535)
#include "SD_MMC.h"
#define LOGBUF_FS SD_MMC
#else
#include "SD.h"
#define LOGBUF_FS SD
#endif

LogBuffer logBuffer;

static const char *kLogFile = "/dash.log";
static const char *kLogOld  = "/dash.old.log";

/* The hook runs on whatever task called log_x(), so the ring is guarded by a
 * spinlock. Formatting happens on the caller's stack, outside the lock. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_chain = nullptr;   /* the original writer (serial) */
static LogBuffer *s_self = nullptr;

/* ESP log lines arrive wrapped in ANSI colour (CONFIG_ARDUHAL_LOG_COLORS=1).
 * Strip the escapes so the on-screen label does not show raw \033[0;32m. */
static void stripAnsi(char *s) {
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '\033') {
            while (*r && *r != 'm') r++;   /* skip to the end of the sequence */
            if (!*r) break;
            continue;
        }
        *w++ = *r;
    }
    *w = '\0';
}

/* ---------------------------------------------------------------------------
 * putc1 hook — the path Arduino's log_i/log_w/log_e actually take.
 *
 * ARDUHAL does NOT route through esp_log_write unless USE_ESP_IDF_LOG is
 * defined (it is not, and defining it would replace the "[file.cpp:76] func():"
 * prefix with a constant ARDUINO tag). Instead log_printfv() ends in
 * ets_printf(), which emits character by character through the ROM's putc1
 * slot — the same slot Serial.setDebugOutput(true) claims for USB CDC.
 *
 * So we claim it after Serial does, forward every character on to Serial so the
 * console is unaffected, and assemble the stream into lines on the side.
 * ------------------------------------------------------------------------ */
static char     s_line[LOGBUF_LINE_LEN];
static uint16_t s_lineLen = 0;
static volatile bool s_inHook = false;

void LogBuffer::putcHook(char c) {
    /* Tap only — do NOT forward to Serial. Installing on this channel does not
     * displace the USB CDC sink (measured: ets_printf still reaches the console
     * with this hook in place), so writing the character on again produces a
     * perfectly doubled log. Capture is purely passive. */
    if (s_inHook || !s_self || !s_self->_ring) return;
    s_inHook = true;

    if (c == '\r') { s_inHook = false; return; }

    bool full = (s_lineLen >= LOGBUF_LINE_LEN - 1);
    if (c != '\n' && !full) {
        s_line[s_lineLen++] = c;
    } else {
        if (s_lineLen) {
            s_line[s_lineLen] = '\0';
            stripAnsi(s_line);
            if (s_line[0]) s_self->push(s_line);
            s_lineLen = 0;
        }
        /* A character that arrives on a FULL buffer starts the continuation
         * line — it must not be dropped. Discarding it is why dash.log shows
         * "(15360 bytes e" / "ch)": the 'a' of "each" fell in the gap. */
        if (c != '\n') s_line[s_lineLen++] = c;
    }

    s_inHook = false;
}

int LogBuffer::vprintfHook(const char *fmt, va_list args) {
    /* Serial first and always: capture must never cost us the existing output,
     * and a failure in here must not swallow the very line explaining it. */
    va_list copy;
    va_copy(copy, args);
    int n = s_chain ? s_chain(fmt, args) : 0;

    if (s_self && s_self->_ring) {
        char line[LOGBUF_LINE_LEN];
        vsnprintf(line, sizeof(line), fmt, copy);
        stripAnsi(line);

        /* Trim the trailing newline; the ring stores one line per slot. */
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';

        if (len) s_self->push(line);
    }
    va_end(copy);
    return n;
}

void LogBuffer::push(const char *line) {
    /* ARDUHAL formats as "[  2933][E][file:line] func(): msg" — the level is the
     * character inside the second bracket pair. Cheaper and more robust than
     * re-deriving severity from the format string. */
    const char *lvl = strstr(line, "][");
    if (lvl && lvl[2]) {
        if      (lvl[2] == 'E') _errors++;
        else if (lvl[2] == 'W') _warnings++;
    }

    taskENTER_CRITICAL(&s_mux);
    char *slot = _ring + (size_t)_head * LOGBUF_LINE_LEN;
    strncpy(slot, line, LOGBUF_LINE_LEN - 1);
    slot[LOGBUF_LINE_LEN - 1] = '\0';
    _head = (_head + 1) % LOGBUF_LINES;
    if (_count < LOGBUF_LINES) _count++;
    _written++;
    taskEXIT_CRITICAL(&s_mux);
}

void LogBuffer::begin() {
    if (_started) return;

    /* 8 KB of ring. Internal DRAM is scarce next to 8 MB of PSRAM, so prefer
     * PSRAM and only fall back to the heap if it is unavailable. */
    size_t bytes = (size_t)LOGBUF_LINES * LOGBUF_LINE_LEN;
    _ring = (char *)ps_malloc(bytes);
    if (!_ring) _ring = (char *)malloc(bytes);
    if (!_ring) return;              /* no buffer: stay a pass-through */
    memset(_ring, 0, bytes);

    s_self = this;

    /* Two hooks, two different pipes, no overlap:
     *  - putc1  catches Arduino's log_x (ets_printf), which is nearly everything
     *           this firmware logs, prefix and all.
     *  - vprintf catches ESP-IDF components that call esp_log_write directly;
     *           those go out through stdout and never reach putc1.            */
    esp_rom_install_channel_putc(1, &LogBuffer::putcHook);
    s_chain = esp_log_set_vprintf(&LogBuffer::vprintfHook);

    _started = true;
}

void LogBuffer::service() {
#if !defined(ENABLE_LOG_TO_SD)
    /* Off by default. Measured on log_11.csv: with this flushing every ~5s the
     * 25Hz telemetry log lost 24% of its rows, including 26 stalls of exactly
     * 240ms (six missed epochs) at about the flush cadence. An open/append/close
     * on FAT rewrites directory and FAT entries, and LogTask's queue is only
     * 1.6s deep. Dropped telemetry costs more than persistent logs are worth, so
     * this is opt-in: build with -D ENABLE_LOG_TO_SD when chasing a fault, and
     * expect the session log to suffer while you do.
     * The RAM ring behind the log screen is unaffected either way. */
    return;
#endif
    if (!_ring || _flushed >= _written) return;

    File f = LOGBUF_FS.open(kLogFile, FILE_APPEND);
    if (!f) return;                  /* card not mounted yet — try again later */

    /* Rotate before it can fill the card. One generation back is enough to
     * cover "what happened last weekend" without unbounded growth. */
    if (f.size() > LOGBUF_MAX_BYTES) {
        f.close();
        LOGBUF_FS.remove(kLogOld);
        LOGBUF_FS.rename(kLogFile, kLogOld);
        f = LOGBUF_FS.open(kLogFile, FILE_APPEND);
        if (!f) return;
    }

    /* Anything older than the ring is already gone; start from the oldest slot
     * still held rather than pretending we can replay it. */
    uint32_t from = _written - (_written - _flushed > _count ? _count : _written - _flushed);
    for (uint32_t i = from; i < _written; i++) {
        uint16_t age  = (uint16_t)(_written - i);           /* 1 = newest */
        uint16_t slot = (uint16_t)((_head + LOGBUF_LINES - age) % LOGBUF_LINES);
        f.println(_ring + (size_t)slot * LOGBUF_LINE_LEN);
    }
    f.close();
    _flushed = _written;
}

size_t LogBuffer::render(char *out, size_t outLen) {
    if (!out || outLen == 0) return 0;
    out[0] = '\0';
    if (!_ring || _count == 0) return 0;

    /* Walk back from the newest line to find how many fit, THEN copy forward.
     * Filling oldest-first and stopping at the end of the buffer would drop the
     * newest lines — the opposite of what a fault report needs. */
    uint16_t fit = 0;
    size_t budget = 0;
    for (uint16_t i = 0; i < _count; i++) {
        uint16_t slot = (uint16_t)((_head + LOGBUF_LINES - 1 - i) % LOGBUF_LINES);
        size_t len = strlen(_ring + (size_t)slot * LOGBUF_LINE_LEN) + 1;  /* + '\n' */
        if (budget + len + 1 >= outLen) break;
        budget += len;
        fit++;
    }

    size_t used = 0;
    for (uint16_t i = 0; i < fit; i++) {
        uint16_t slot = (uint16_t)((_head + LOGBUF_LINES - fit + i) % LOGBUF_LINES);
        const char *line = _ring + (size_t)slot * LOGBUF_LINE_LEN;
        size_t len = strlen(line);
        memcpy(out + used, line, len);
        used += len;
        out[used++] = '\n';
    }
    out[used] = '\0';
    return used;
}

void LogBuffer::acknowledge() {
    _errors = 0;
    _warnings = 0;
}

void LogBuffer::clear() {
    taskENTER_CRITICAL(&s_mux);
    _head = _count = 0;
    _written = _flushed = 0;
    taskEXIT_CRITICAL(&s_mux);
    _errors = _warnings = 0;
    LOGBUF_FS.remove(kLogFile);
    LOGBUF_FS.remove(kLogOld);
}
