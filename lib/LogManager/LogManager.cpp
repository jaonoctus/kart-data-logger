#include "LogManager.h"

#include <sys/time.h>
#include <time.h>

namespace {
constexpr uint64_t kMinValidGpsEpochMs = 1609459200000ULL; // 2021-01-01 UTC

/* How often the open log file is pushed to the card. This is the whole cost of
 * holding the file open: an abrupt power-off loses up to this much of the lap,
 * where the old open-append-close per row lost at most one sample. Bought
 * deliberately — 25Hz means 25 FAT directory updates a second otherwise, which
 * is where a slow card starts stalling and dropping frames. */
constexpr uint32_t kFlushIntervalMs = 1000;

/* Dropped frames are checked at this interval, and a row is written only when the
 * count has moved. A clean race therefore leaves /health.csv untouched: any row in
 * it means the card could not keep up, which is the whole point of persisting it —
 * the serial warning is no use with the dash bolted to a steering wheel. */
constexpr uint32_t kHealthCheckIntervalMs = 1000;
constexpr const char *kHealthFileName = "/health.csv";
}

QueueHandle_t LogManager::logQueue = NULL;

LogManager::LogManager() {}

bool LogManager::isSdAvailable() const {
    return _sdAvailable;
}

#if !defined(JC3248W535)
bool LogManager::begin(SemaphoreHandle_t spiMutex) {
    _spiMutex = spiMutex;
#else
bool LogManager::begin() {
#endif
    // Ensure SD CS is High (Disabled) before we even start
    #if !defined(JC3248W535)
    pinMode(TF_CS, OUTPUT); 
    digitalWrite(TF_CS, HIGH);
    #endif

    /* 40 frames is ~1.6s of headroom at 25Hz (20 was 2s at 10Hz, but only 800ms at
     * 25). TelemetryMsg is 51 bytes packed, so this costs ~1KB of internal DRAM,
     * which is scarce here — but overflowing the queue drops rows, and dropped rows
     * are the one thing the higher fix rate was bought to avoid. */
    logQueue = xQueueCreate(40, sizeof(TelemetryMsg));

    // Try to initialize SD card
    #if defined(JC3248W535)
    SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);
    if (!SD_MMC.begin("/sdmmc", true, false, 20000)) {
        log_e("SD Card Mount Failed");
        _sdAvailable = false;
    } else {
        log_i("LogManager: SD Card Initialized.");
        _sdAvailable = true;
    }
    #else
    if (xSemaphoreTake(_spiMutex, pdMS_TO_TICKS(1000))) {
        // CYD SD is usually on VSPI (Pins 5, 18, 19, 23)
        if (!SD.begin(TF_CS)) {
            log_e("SD Card Mount Failed on Pin %d", TF_CS);
            _sdAvailable = false;
        } else {
            log_i("LogManager: SD Card Initialized.");
            _sdAvailable = true;
        }
        xSemaphoreGive(_spiMutex);
    }
    #endif

    xTaskCreatePinnedToCore(LogManager::task, "LogTask", 4096, this, 1, NULL, 0);
    return _sdAvailable;
}

void LogManager::createNewFile() {
    // Basic incrementing file name: log_0.csv, log_1.csv...
    int i = 0;
    #if defined(JC3248W535)
    while (SD_MMC.exists("/log_" + String(i) + ".csv")) i++;
    #else
    while (SD.exists("/log_" + String(i) + ".csv")) i++;
    #endif
    _currentFileName = "/log_" + String(i) + ".csv";

    #if defined(JC3248W535)
    _file = SD_MMC.open(_currentFileName, FILE_WRITE);
    #else
    _file = SD.open(_currentFileName, FILE_WRITE);
    #endif
    if (_file) {
        /* fix/pdop/hacc/sacc are appended, never inserted — tools/overlay and
         * tools/lapreplay read the first nine by name or by a fixed sscanf prefix. */
        _file.println("epoch,speed,totalGForce,gForceX,gForceY,steering_angle,sats,lat,lng,"
                      "fix,pdop,hacc,sacc"); //CSV header
        _file.flush();
        _lastFlushMs = millis();
        log_i("LogManager: Started new log: %s", _currentFileName.c_str());
    } else {
        log_e("LogManager: could not open %s for writing.", _currentFileName.c_str());
    }
}

void LogManager::closeFile() {
    if (!_file) return;

    #if !defined(JC3248W535)
    bool locked = xSemaphoreTake(_spiMutex, pdMS_TO_TICKS(1000));
    if (!locked) log_w("LogManager: SPI timeout on close; the last rows may be lost.");
    #endif

    _file.flush();
    _file.close();

    #if !defined(JC3248W535)
    if (locked) xSemaphoreGive(_spiMutex);
    #endif
}

/* One row per second in which frames were lost, appended across every session so a
 * race weekend accumulates in one place. Opened and closed per row — unlike the
 * telemetry file this is rare by design, and a row that is not on the card when the
 * kart is switched off is a row that never existed. */
void LogManager::writeHealthRow(uint32_t drops, uint32_t delta) {
    #if !defined(JC3248W535)
    if (!xSemaphoreTake(_spiMutex, pdMS_TO_TICKS(500))) return;
    #endif

    #if defined(JC3248W535)
    bool fresh = !SD_MMC.exists(kHealthFileName);
    File h = SD_MMC.open(kHealthFileName, FILE_APPEND);
    #else
    bool fresh = !SD.exists(kHealthFileName);
    File h = SD.open(kHealthFileName, FILE_APPEND);
    #endif

    if (h) {
        if (fresh) h.println("epoch,log,dropped_total,dropped_delta");
        /* Wall clock, not millis(): the log task syncs the system clock from GPS, so
         * this lines up with the epoch column in the telemetry file. Falls back to
         * uptime-since-1970 before the first fix, which is obvious enough on sight. */
        h.printf("%llu,%s,%lu,%lu\n",
                 (uint64_t)time(nullptr) * 1000ULL,
                 _currentFileName.isEmpty() ? "-" : _currentFileName.c_str(),
                 (unsigned long)drops, (unsigned long)delta);
        h.flush();
        h.close();
    }

    #if !defined(JC3248W535)
    xSemaphoreGive(_spiMutex);
    #endif
}

/* Locks for itself so it can be called from the idle branch too — without that, a
 * session whose fixes stop coming (kart parked, or no sats) leaves its last rows
 * sitting unflushed until it happens to receive another frame. */
void LogManager::flushIfDue() {
    if (!_file) return;
    if (millis() - _lastFlushMs < kFlushIntervalMs) return;

    #if !defined(JC3248W535)
    if (!xSemaphoreTake(_spiMutex, pdMS_TO_TICKS(500))) return;
    #endif

    _file.flush();
    _lastFlushMs = millis();

    #if !defined(JC3248W535)
    xSemaphoreGive(_spiMutex);
    #endif
}

void LogManager::startSession() {
    if (!_sdAvailable || _sessionActive || _pendingStart) return;
    _pendingStart = true;
    log_i("LogManager: session start requested.");
}

void LogManager::stopSession() {
    if (!_sessionActive && !_pendingStart) return;
    _pendingStart = false;
    _pendingStop = true;
    log_i("LogManager: session stop requested.");
}

bool LogManager::hasValidGpsTime(const TelemetryMsg &msg) const {
    return msg.sats > 0 && msg.timestamp >= kMinValidGpsEpochMs;
}

bool LogManager::syncClockFromTelemetry(const TelemetryMsg &msg) {
    if (!hasValidGpsTime(msg)) return false;

    struct timeval tv = {};
    tv.tv_sec = (time_t)(msg.timestamp / 1000ULL);
    tv.tv_usec = (suseconds_t)((msg.timestamp % 1000ULL) * 1000ULL);

    if (settimeofday(&tv, nullptr) != 0) {
        log_w("LogManager: failed to sync system time from GPS epoch %llu.", msg.timestamp);
        return false;
    }

    if (!_clockSynced) log_i("LogManager: system time synced from GPS.");
    _clockSynced = true;
    return true;
}

void LogManager::task(void* param) {
    LogManager* self = (LogManager*)param;
    TelemetryMsg msg;

    for (;;) {
        /* Health accounting first, so it still runs when the queue is empty — a card
         * stalling badly enough to drop frames is also a card that may have stopped
         * delivering them. */
        if (millis() - self->_lastHealthCheckMs >= kHealthCheckIntervalMs) {
            self->_lastHealthCheckMs = millis();
            uint32_t drops = self->_droppedFrames;
            if (self->_sdAvailable && drops != self->_lastHealthDrops) {
                self->writeHealthRow(drops, drops - self->_lastHealthDrops);
                self->_lastHealthDrops = drops;
            }
        }

        // Process pending start/stop requests (flags set by any task).
        if (self->_pendingStop) {
            self->_pendingStop   = false;
            self->_sessionActive = false;
            self->closeFile();
            self->_currentFileName = "";
            log_i("LogManager: session stopped.");
        }
        if (self->_pendingStart && !self->_sessionActive) {
            self->_pendingStart = false;
            self->createNewFile();
            self->_sessionActive = true;
            log_i("LogManager: session active.");
        }

        // Use a timeout so we can re-check flags even when the queue is empty.
        if (!xQueueReceive(logQueue, &msg, pdMS_TO_TICKS(100))) {
            self->flushIfDue();
            continue;
        }

        if (!self->_sdAvailable || !self->_sessionActive) continue;
        if (msg.sats == 0) continue;
        if (!self->_file) continue;

        // Best-effort clock sync from GPS (non-blocking; doesn't gate logging).
        if (!self->_clockSynced) self->syncClockFromTelemetry(msg);

        #if !defined(JC3248W535)
        if (!xSemaphoreTake(self->_spiMutex, pdMS_TO_TICKS(500))) {
            log_w("LogManager: SPI Timeout! Missed a log entry.");
            continue;
        }
        #endif

        /* lat/lng at 6 decimal places is ~11cm, which was already coarser than NMEA's
         * 1.85cm and is far coarser than NAV-PVT's 1.1cm. 7 places keeps what the
         * receiver actually resolves; the extra byte per row is not the bottleneck. */
        self->_file.printf("%llu,%.1f,%.2f,%.2f,%.2f,%.1f,%d,%.7f,%.7f,%u,%.2f,%.2f,%.2f\n",
                           msg.timestamp, msg.speedKmph, msg.totalGForce,
                           msg.gForceX, msg.gForceY, msg.steeringAngle, msg.sats, msg.lat, msg.lng,
                           msg.fixType, msg.pdop, msg.hAccM, msg.sAccMps);

        #if !defined(JC3248W535)
        xSemaphoreGive(self->_spiMutex);
        #endif

        // After the release, because flushIfDue() takes the mutex itself.
        self->flushIfDue();
    }
}