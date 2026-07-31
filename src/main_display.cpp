#include <Arduino.h>
#include "esp_task_wdt.h"   // display-wedge safety net (used throughout)
#include "esp_system.h"     // esp_reset_reason

// QSPI flush telemetry from lib/DisplayBSP/lv_port.c (C linkage). Reported once a
// second below via log_w so it surfaces at CORE_DEBUG_LEVEL=WARN.
extern "C" {
    extern volatile uint32_t lvgl_port_frame_drops;
    extern volatile uint32_t lvgl_port_max_stall_us;
}

// Library includes from our new /lib folder
#include "EspNowManager.h"
#include "LogManager.h"
#include "EspNowProtocol.h"
#include "uiHelper.h"
#include "BatteryManager.h"
#include "ConfigManager.h"
#include "LapManager.h"

#if defined(ENABLE_IMU)
#include "ImuManager.h"
#include "CalibrationManager.h"

#define I2C_SDA 18
#define I2C_SCL 17
#endif

#ifndef HAS_HELMET
#include "GpsManager.h"
#endif

#if defined(GPS_PROVIDER_ATGM336)
const uint8_t expectedPps = 10;
#else
const uint8_t expectedPps = 5;
#endif

const uint8_t timeBetweenImuUplinkMessages = 10; // ms between IMU reads/uplink (100 Hz)

// --- GLOBAL STATE ---
LogManager logManager;
UiHelper   uiHelper;
LapManager lapManager;

#ifndef HAS_HELMET
GpsManager gps(GPS_STANDALONE_RX, GPS_STANDALONE_TX);
#endif

#if defined(ENABLE_IMU)
ImuManager imu;
CalibrationManager calibManager(imu);
ImuData latestImuData = {0};
bool imuReady = false;
uint32_t lastImuReadMs = 0;
#endif

// Display battery: BAT+ → 68k (683) → IO5 → 100k (01D) → GND (always-on divider)
// Theoretical ratio (68+100)/100 = 1.68, but empirical 4.224/2.47 = 1.71 absorbs the ~30mV ADC offset.
#define DISPLAY_BATT_ADC 5
#define DISPLAY_BATT_READ_INTERVAL_MS 5000
BatteryManager displayBattery(DISPLAY_BATT_ADC, 0xFF, 4.224f / 2.47f);
static uint32_t lastDisplayBattReadMs = 0;

// Smoothing & Metrics
float displaySpeed = 0;
float targetSpeed = 0;
const float lerpFactor = 0.20f;
uint32_t lastMessageCount = 0;
uint32_t pps = 0;
uint32_t lastPPSUpdate = 0;
#ifdef HAS_HELMET
uint8_t helmetBatteryCurrentLevel = 255;
#endif

// Lap state — updated inside syncUI(), consumed in loop() after the display lock
static bool     s_lapCompletedPending = false;
static LapCompletedMsg s_pendingLapMsg = {};
static uint8_t  s_lapCount = 0;

#ifdef HAS_HELMET
// Error log transfer state (logger -> display -> SD)
static bool errorLogReceiving = false;
static uint16_t errorLogExpectedLines = 0;
static uint16_t errorLogWrittenLines = 0;
static bool errorLogWriteFailed = false;

static void processIncomingErrorLog() {
    uint16_t totalLines = 0;
    if (EspNowManager::consumeErrorLogStart(totalLines)) {
        errorLogReceiving = true;
        errorLogExpectedLines = totalLines;
        errorLogWrittenLines = 0;
        errorLogWriteFailed = false;

        if (!logManager.beginHelmetErrorLog()) {
            errorLogWriteFailed = true;
            log_w("Error log start failed in LogManager.");
        }

        log_i("Receiving helmet_error.log with %u lines.", totalLines);
    }

    ErrorLogLineMsg lineMsg = {};
    while (EspNowManager::popErrorLogLine(lineMsg)) {
        esp_task_wdt_reset(); // SD writes here run on loopTask; keep the WDT fed
        if (!errorLogReceiving || errorLogWriteFailed) {
            continue;
        }

        if (!logManager.appendHelmetErrorLine(lineMsg.lineData)) {
            errorLogWriteFailed = true;
            log_w("Error log line write failed in LogManager.");
            continue;
        }

        errorLogWrittenLines++;
    }

    if (EspNowManager::consumeErrorLogEnd(totalLines)) {
        uint16_t dropped = EspNowManager::getErrorLogDroppedLines();
        bool acked = errorLogReceiving &&
                     logManager.finalizeHelmetErrorLogAndAck(errorLogExpectedLines, errorLogWrittenLines, dropped, errorLogWriteFailed);

        if (!acked) {
            log_w("Error log incomplete. expected=%u written=%u dropped=%u", errorLogExpectedLines, errorLogWrittenLines, dropped);
        }

        errorLogReceiving = false;
    }
}
#endif // HAS_HELMET

// ============================================================================
// DISPLAY-WEDGE SAFETY NET (ALWAYS ON) - "indestructible" recovery.
// The AXS15231B QSPI panel write (panel_axs15231b_draw_bitmap) can occasionally
// hang the LVGL task while it holds the display lock, which would otherwise
// freeze the whole UI forever (screen + touch + serial dead). The spin cannot
// be safely unstuck in place, so we guarantee recovery instead: a short Task
// Watchdog on loopTask converts a wedge into a fast auto-reset (~5s) and the
// dash comes straight back.
// Timeout keeps headroom over legitimate slow paths (SD writes) so it only ever
// fires on a genuine *infinite* wedge, never on a transient stall.
// ============================================================================
#define DISPLAY_WDT_TIMEOUT_MS 5000

// Recovery counter persisted across the panic-reset (RTC memory survives a SW
// reset; it is garbage on a true power-on, so guard it with a magic value).
RTC_NOINIT_ATTR static uint32_t s_recoveryMagic;
RTC_NOINIT_ATTR static uint32_t s_recoveryCount;

static void displayWdtInit() {
    esp_task_wdt_config_t twdt = {};
    twdt.timeout_ms     = DISPLAY_WDT_TIMEOUT_MS;
    twdt.idle_core_mask = 0;
    twdt.trigger_panic  = true;   // a genuine loopTask wedge -> fast auto-reset
    if (esp_task_wdt_reconfigure(&twdt) != ESP_OK) {
        esp_task_wdt_init(&twdt);   // not yet initialized by the Arduino core
    }
    esp_task_wdt_add(NULL);          // subscribe loopTask; fed each loop()
}

static void reportRecoveryOnBoot() {
    esp_reset_reason_t rr = esp_reset_reason();
    if (s_recoveryMagic != 0xC0FFEE42u) {   // fresh power-on: RTC RAM is garbage
        s_recoveryMagic = 0xC0FFEE42u;
        s_recoveryCount = 0;
    }
    if (rr == ESP_RST_TASK_WDT || rr == ESP_RST_INT_WDT ||
        rr == ESP_RST_WDT      || rr == ESP_RST_PANIC) {
        s_recoveryCount++;
        log_w("=== AUTO-RECOVERED from display wedge (reset_reason=%d, recoveries this power cycle=%u) ===",
              (int)rr, s_recoveryCount);
    } else {
        s_recoveryCount = 0; // normal power-on / flash / external reset
    }
}

// ============================================================================
// BRIDGES (called from LVGL event callbacks via ui_theme.cpp)
// Already in LVGL task context — do NOT call bsp_display_lock here.
// ============================================================================
extern "C" void ui_helper_apply_finish_line(double ll, double ln, double rl, double rn) {
    FinishLine fl = { ll, ln, rl, rn };
    lapManager.setFinishLine(fl);
    s_lapCount = 0; // Reset lap counter when finish line changes
}

extern "C" bool ui_helper_get_gps(double *lat, double *lon) {
    if (!EspNowManager::lastTelemetry.hasFix) return false;
    *lat = EspNowManager::lastTelemetry.lat;
    *lon = EspNowManager::lastTelemetry.lng;
    return true;
}

extern "C" void ui_helper_toggle_session() {
    if (logManager.isSessionActive()) {
        logManager.stopSession();
        uiHelper.setSessionState(false);
    } else {
        logManager.startSession();
        uiHelper.setSessionState(true);

        // Re-apply the active track's finish line to lapManager on session start
        int sel = (int)configManager.getSelectedTrack();
        const TrackConfig *active = configManager.getTrack(sel);
        if (active && active->left_valid && active->right_valid) {
            FinishLine fl = { active->left_lat, active->left_lon,
                              active->right_lat, active->right_lon };
            lapManager.setFinishLine(fl);
            s_lapCount = 0;
        }
    }
}

extern "C" void ui_helper_stop_session() {
    logManager.stopSession();
    uiHelper.setSessionState(false);
}

void syncUI() {
    bsp_display_lock(0);
    // 1. Get data from Radio
    if (EspNowManager::newDataAvailable) {
        EspNowManager::newDataAvailable = false;

        // Inject current steering angle before logging/processing
#if defined(ENABLE_IMU)
        if (imuReady) {
            EspNowManager::lastTelemetry.steeringAngle = calibManager.getSteeringAngle();

            uiHelper.setGx(EspNowManager::lastTelemetry.gForceX);
            uiHelper.setGy(EspNowManager::lastTelemetry.gForceY);
        }
#else
        uiHelper.setGx(EspNowManager::lastTelemetry.gForceX);
        uiHelper.setGy(EspNowManager::lastTelemetry.gForceY);
#endif

        // 2. Push to SD Log Queue — only when a session is active to avoid stale data
        if (LogManager::logQueue != NULL && logManager.isSessionActive()) {
            xQueueSend(LogManager::logQueue, &EspNowManager::lastTelemetry, 0);
        }

        // lastProcessedTimestamp = EspNowManager::lastTelemetry.timestamp;
        lastMessageCount++;

        uiHelper.setSpeed(EspNowManager::lastTelemetry.speedKmph);

        // 8. Update GPS Satellite Indicator
        uint8_t sats = EspNowManager::lastTelemetry.sats;
        static uint8_t lastSats = 255; // Use 255 so it guarantees an update on the very first loop
        if (sats != lastSats) {
            lastSats = sats;
            
            uiHelper.setGps(lastSats);
        }

        // 9. Update Helmet Battery Level
#ifdef HAS_HELMET
        static uint8_t lastBatteryLevel = 255;
        helmetBatteryCurrentLevel = EspNowManager::lastTelemetry.helmetBattery;
        if (helmetBatteryCurrentLevel >= 100) helmetBatteryCurrentLevel = 100;
        if (helmetBatteryCurrentLevel != lastBatteryLevel) {
            lastBatteryLevel = helmetBatteryCurrentLevel;
            uiHelper.setHelmet(lastBatteryLevel);
        }
#endif

        // 10. Lap Detection — only while a session is active
        if (logManager.isSessionActive() && lapManager.processTelemetry(EspNowManager::lastTelemetry)) {
            s_lapCount++;
            uint64_t lt = lapManager.getLastLapTime();
            uint64_t bt = lapManager.getBestLapTime();
            uint64_t pt = lapManager.getPreviousLapTime();
            bool isBest = (lt == bt && bt != 0 && bt != 0xFFFFFFFFFFFFFFFFULL);

            char lapStr[20], bestStr[20];
            int ltMin = (int)(lt / 60000), ltSec = (int)((lt % 60000) / 1000), ltMs = (int)(lt % 1000);
            if (ltMin > 0)
                snprintf(lapStr, sizeof(lapStr), "%d:%02d.%03d", ltMin, ltSec, ltMs);
            else
                snprintf(lapStr, sizeof(lapStr), "%d.%03d", ltSec, ltMs);

            if (bt != 0xFFFFFFFFFFFFFFFFULL) {
                int btMin = (int)(bt / 60000), btSec = (int)((bt % 60000) / 1000), btMs = (int)(bt % 1000);
                if (btMin > 0)
                    snprintf(bestStr, sizeof(bestStr), "%d:%02d.%03d", btMin, btSec, btMs);
                else
                    snprintf(bestStr, sizeof(bestStr), "%d.%03d", btSec, btMs);
            } else {
                bestStr[0] = '\0';
            }

            uiHelper.setLap(s_lapCount, lapStr, bestStr);

            if (pt > 0) {
                int64_t deltaMs = (int64_t)lt - (int64_t)pt;
                uiHelper.setDelta(fabsf((float)deltaMs / 1000.0f), deltaMs <= 0);
            }

            s_pendingLapMsg.type           = MSG_LAP_COMPLETED;
            s_pendingLapMsg.lapTimeMs      = lt;
            s_pendingLapMsg.previousLapTimeMs = pt;
            s_pendingLapMsg.bestLapTimeMs  = bt;
            s_pendingLapMsg.isBest         = isBest;
            s_lapCompletedPending = true;

            log_i("Lap %d completed: %s (best: %s)", s_lapCount, lapStr, bestStr);
        }
    }

    // --- SIGNAL HEALTH INDICATOR (helmet link only) ---
    uiHelper.setPps(expectedPps, pps);

    // Drive the recording panel blink
    uiHelper.tickRecordingPanel();

    bsp_display_unlock();
}

void setup() {
    Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT == 1
    delay(2000);
#endif
    Serial.setDebugOutput(true);
#if ARDUINO_USB_CDC_ON_BOOT == 1
    // Non-blocking USB-CDC TX: with no host draining the port (the normal case in
    // the kart), the CDC buffer fills and log_*() would otherwise BLOCK the calling
    // task — freezing the dash until a serial monitor reconnects. Drop instead.
    Serial.setTxTimeoutMs(0);
#endif

    log_i("--- KART DISPLAY BOOTING ---");

    // Report whether this boot is a normal start or an auto-recovery from a
    // display wedge (Option 4 safety net).
    reportRecoveryOnBoot();

    // 1. Initialize IMU FIRST
#if defined(ENABLE_IMU)
    log_i("Initializing IMU on SDA:%d SCL:%d...", I2C_SDA, I2C_SCL);

    // We pass the pins directly to imu.begin to handle bus recovery and Wire.begin internally
    if (imu.begin(I2C_SDA, I2C_SCL, 100000)) {
        log_i("IMU: OK");
        imuReady = true;
    } else {
        log_e("IMU: Error - Check wiring on expansion port (GPIO 9/10)");
    }
#else
    log_i("Display IMU disabled (ENABLE_IMU not defined)");
#endif

    // 2. Initialize UI
    log_i("Display BSP init...");
    uiHelper.init();

    // 3. Initialize Managers
#ifdef HAS_HELMET
    EspNowManager::begin();
#endif
    logManager.begin();
    displayBattery.begin();

#ifndef HAS_HELMET
    if (!gps.begin()) {
        log_e("Standalone GPS failed to start on RX=%d TX=%d!", GPS_STANDALONE_RX, GPS_STANDALONE_TX);
    }
#endif

    // 4. Load config from SD and apply saved theme
    configManager.begin();
    bsp_display_lock(0);
    uiHelper.setTheme(configManager.getTheme() == 0 ? DASH_MODE_NIGHT : DASH_MODE_DAY);
    bsp_display_unlock();

    // 4. Load config + tracks from SD and apply to UI
    configManager.begin();
    bsp_display_lock(0);

    uiHelper.setTheme(configManager.getTheme() == 0 ? DASH_MODE_NIGHT : DASH_MODE_DAY);

    // Build name pointer array directly into ConfigManager's stable storage
    static const char *track_name_ptrs[CONFIG_MAX_TRACKS];
    int track_count = configManager.getTrackCount();
    for (int i = 0; i < track_count; i++) {
        const TrackConfig *t = configManager.getTrack(i);
        track_name_ptrs[i] = t ? t->name : "";
    }
    uiHelper.setTracks(track_name_ptrs, track_count);

    int sel = (int)configManager.getSelectedTrack();
    uiHelper.setTrackIdx(sel);

    const TrackConfig *active = configManager.getTrack(sel);
    if (active) {
        uiHelper.setStartL(active->left_lat, active->left_lon, active->left_valid);
        uiHelper.setStartR(active->right_lat, active->right_lon, active->right_valid);
        if (active->left_valid && active->right_valid) {
            FinishLine fl = { active->left_lat, active->left_lon,
                              active->right_lat, active->right_lon };
            lapManager.setFinishLine(fl);
        }
    }

#ifndef HAS_HELMET
    uiHelper.hideHelmet();
#endif

    bsp_display_unlock();

#if defined(ENABLE_IMU)
    if (imuReady) {
        calibManager.begin();
    }
#endif

    // Display-wedge safety net (always on): a wedge -> ~5s auto-reset, not a freeze.
    displayWdtInit();
    log_i("Display watchdog armed: loopTask WDT=%dms (panic+reset on wedge)", DISPLAY_WDT_TIMEOUT_MS);

    log_i("Display System Ready.");
}

void loop() {
    uint32_t now = millis();

    esp_task_wdt_reset();   // feed the display-wedge watchdog (always on)

#ifdef HAS_HELMET
    processIncomingErrorLog();
#endif

#if defined(ENABLE_IMU)
    if (imuReady) {
        float currentSteering = calibManager.getSteeringAngle();

        if (calibManager.isDone() && (now - lastImuReadMs >= timeBetweenImuUplinkMessages)) {
            latestImuData = imu.update(currentSteering);
            lastImuReadMs = now;

#ifdef HAS_HELMET
            // Push IMU sample to helmet so it can use it for GPS speed filtering.
            ImuFeedbackMsg imuMsg = {};
            imuMsg.type = MSG_IMU_FEEDBACK;
            imuMsg.gForceX = latestImuData.accelX;
            imuMsg.gForceY = latestImuData.accelY;
            imuMsg.totalGForce = latestImuData.gForce;
            imuMsg.gyroZ = latestImuData.gyroZ;
            imuMsg.sampleMs = now;
            EspNowManager::sendImuFeedback(imuMsg);
#endif
        }
    }
#endif

#ifndef HAS_HELMET
    // Poll local GPS and inject it as telemetry (reuses the helmet-mode processing path).
    if (gps.update()) {
        float imuG = 0.0f, imuGyroZ = 0.0f, imuGx = 0.0f, imuGy = 0.0f;
#if defined(ENABLE_IMU)
        if (imuReady) {
            imuG     = latestImuData.gForce;
            imuGyroZ = latestImuData.gyroZ;
            imuGx    = latestImuData.accelX;
            imuGy    = latestImuData.accelY;
        }
#endif
        EspNowManager::lastTelemetry.type        = MSG_TELEMETRY;
        EspNowManager::lastTelemetry.speedKmph   = (float)gps.getSpeed(imuG, imuGyroZ);
        EspNowManager::lastTelemetry.gForceX     = imuGx;
        EspNowManager::lastTelemetry.gForceY     = imuGy;
        EspNowManager::lastTelemetry.totalGForce = imuG;
        EspNowManager::lastTelemetry.gyroZ       = imuGyroZ;
        EspNowManager::lastTelemetry.lat         = gps.getLat();
        EspNowManager::lastTelemetry.lng         = gps.getLng();
        EspNowManager::lastTelemetry.sats        = (uint8_t)gps.getSatellites();
        EspNowManager::lastTelemetry.hasFix      = gps.hasFix() ? 1 : 0;
        EspNowManager::lastTelemetry.timestamp   = gps.getEpochMs();
        EspNowManager::newDataAvailable          = true;
    }
#endif

    // Display battery ADC read — done before the display lock to avoid blocking LVGL
    static uint8_t cachedDisplayBattPct = 255;
    static uint8_t lastDisplayBattPct = 255;
    if (now - lastDisplayBattReadMs >= DISPLAY_BATT_READ_INTERVAL_MS) {
        lastDisplayBattReadMs = now;
        cachedDisplayBattPct = (uint8_t)displayBattery.getPercentage();
        log_i("Display battery: %d%%", cachedDisplayBattPct);
    }

    // PPS Counter Logic
    if (now - lastPPSUpdate >= 1000) {
        uint32_t lastPps = lastMessageCount;
        lastMessageCount = 0;
        if (pps != lastPps) pps = lastPps;
        lastPPSUpdate = now;

        // QSPI flush health: report dropped frames and the worst bus-idle wait in
        // the last second, then reset the stall high-water mark. A blink should line
        // up with either a frame_drops increment (hard skip) or a high max_stall.
        static uint32_t lastFrameDrops = 0;
        uint32_t drops = lvgl_port_frame_drops;
        uint32_t stallUs = lvgl_port_max_stall_us;
        lvgl_port_max_stall_us = 0;
        if (drops != lastFrameDrops || stallUs >= 8000) {
            log_w("[DISP] frame_drops=%lu (+%lu)  max_bus_stall=%lu us",
                  (unsigned long)drops, (unsigned long)(drops - lastFrameDrops),
                  (unsigned long)stallUs);
            lastFrameDrops = drops;
        }

#if defined(ENABLE_IMU)
        if (imuReady) {
            float steeringDeg = calibManager.getSteeringAngle();
            latestImuData = imu.update(steeringDeg);
            log_i("Incoming Rate: %d pkts/sec | Steering: %.1f deg | G: %.2f\n", pps, steeringDeg, latestImuData.gForce);
        } else {
            log_i("Incoming Rate: %d pkts/sec\n", pps);
        }
#else
        log_i("Incoming Rate: %d pkts/sec\n", pps);
#endif
    }

    bsp_display_lock(0);

#if defined(ENABLE_IMU)
    if (imuReady) {
        calibManager.update();
    }
#endif

    if (cachedDisplayBattPct != lastDisplayBattPct) {
        lastDisplayBattPct = cachedDisplayBattPct;
        uiHelper.setDisplay(cachedDisplayBattPct);
    }

    // Update UI Elements
    syncUI();

    bsp_display_unlock();

#ifdef HAS_HELMET
    // Send lap completed notification to helmet outside the display lock
    if (s_lapCompletedPending) {
        s_lapCompletedPending = false;
        EspNowManager::sendLapCompleted(s_pendingLapMsg);
    }
#else
    s_lapCompletedPending = false; // no helmet to notify; discard
#endif

    delay(1);
}