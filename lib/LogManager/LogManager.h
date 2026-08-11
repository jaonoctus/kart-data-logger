#ifndef LOG_MANAGER_H
#define LOG_MANAGER_H

#include <Arduino.h>

#if defined(JC3248W535)
#include "SD_MMC.h"
#else
#include <SD.h>
#include <SPI.h>
#endif

#include "EspNowProtocol.h"

class LogManager {
public:
    LogManager();
    
    #if defined(JC3248W535)
    bool begin();
    #else
    // Pass the shared SPI Mutex here
    bool begin(SemaphoreHandle_t spiMutex);
    #endif

    // The background task that writes to SD
    static void task(void* param);

    bool isSdAvailable() const;

    /* Called from the producer when the queue had no room for a frame. Counted here
     * rather than at the call site so the log task — the only thing that touches the
     * card — can persist it to /health.csv without a second owner. */
    /* Explicit read-modify-write, not ++: C++20 deprecates increment on a volatile
     * ([-Wvolatile]). Not atomic either way — the producer increments and the log task
     * reads — but losing one count on a dropped-frame tally is harmless, and volatile
     * matches how the session flags below cross tasks. */
    void noteDroppedFrame() { _droppedFrames = _droppedFrames + 1; }
    uint32_t droppedFrames() const { return _droppedFrames; }

    // Session control — call from any task; file I/O happens inside the log task.
    void startSession();
    void stopSession();
    bool isSessionActive() const { return _sessionActive; }

    // Queue that carries telemetry frames from the main loop to the log task
    static QueueHandle_t logQueue;

private:
    #if !defined(JC3248W535)
    SemaphoreHandle_t _spiMutex;
    #endif
    bool _sdAvailable = false;
    bool _clockSynced = false;
    String _currentFileName;

    /* Held open for the whole session rather than reopened per row. Only the log
     * task touches it. */
    File _file;
    uint32_t _lastFlushMs = 0;

    /* Dropped-frame accounting. Incremented by the producer, persisted by the log
     * task. volatile for the cross-task read, like the session flags above. */
    volatile uint32_t _droppedFrames = 0;
    uint32_t _lastHealthDrops  = 0;
    uint32_t _lastHealthCheckMs = 0;

    void writeHealthRow(uint32_t drops, uint32_t delta);

    // Session state — written by log task, flags set by any task (volatile for cross-task visibility)
    volatile bool _sessionActive  = false;
    volatile bool _pendingStart   = false;
    volatile bool _pendingStop    = false;

    void createNewFile();
    void closeFile();
    void flushIfDue();
    bool hasValidGpsTime(const TelemetryMsg &msg) const;
    bool syncClockFromTelemetry(const TelemetryMsg &msg);
};

#endif