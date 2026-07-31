#ifndef GOPRO_MANAGER_H
#define GOPRO_MANAGER_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/* ============================================================================
 * GoProManager — BLE remote control + telemetry for a GoPro action camera.
 *
 * Speaks the GoPro "Control & Query" GATT service (0xFEA6). The protocol is the
 * same family Open GoPro documents for HERO9+, but the subset used here is the
 * one verified on a HERO8 Black: command/query TLVs, the 0x53 "activate
 * information updates" registration, and the multi-packet reassembly header.
 *
 * What we get out of it:
 *   - camera battery percentage   (status 0x46 / 70)
 *   - camera GPS lock             (status 0x44 / 68)
 *   - recording state + duration  (status 0x0A / 10, 0x0D / 13)
 *   - shutter start/stop          (command 0x01)
 *   - wake from sleep             (implicit: connecting boots a sleeping camera)
 *
 * Threading: all BLE work happens on a dedicated task. The dashboard reads a
 * snapshot through status(), which is copied under a critical section, so it is
 * safe to call from loop() / LVGL context.
 *
 * Radio policy: the link exists ONLY while a session wants the camera rolling.
 * Idle means completely off the air — no scanning, no connecting — because
 * connecting is itself what wakes a sleeping GoPro. Session start scans, wakes
 * the camera and starts the clip; session stop rolls the shutter off and then
 * drops the link so the camera can hit its own auto-off timer. The consequence
 * is that camera battery / GPS read "--" on the dash between sessions; that is
 * the price of leaving the camera asleep.
 * ========================================================================= */

struct GoProStatus {
    bool     linked;        /* GATT connected and characteristics resolved     */
    bool     recording;     /* status 10 (encoding)                            */
    bool     busy;          /* status 8 — on HERO8 this mirrors recording, it
                             * is NOT the HERO9+ "system busy" flag. Diagnostic
                             * only; never gate commands on it.                */
    bool     gpsLock;       /* status 68                                       */
    uint8_t  batteryPct;    /* status 70, 0-100; 255 = unknown                 */
    uint32_t recSeconds;    /* status 13, current clip duration                */
};

class GoProManager {
public:
    /* Starts the BLE stack and the worker task. nameFilter, when non-null, is a
     * prefix match against the advertised name (e.g. "GoPro 1234") so a paddock
     * full of cameras still binds to the right one. */
    bool begin(const char *nameFilter = nullptr);

    /* Desired recording state. The worker reconciles this against the camera's
     * reported state, connecting (and thereby waking) the camera if needed, so
     * this is safe to call from a UI callback — it never blocks. */
    void setRecording(bool on);
    bool desiredRecording() const { return m_desiredRec; }

    /* Thread-safe snapshot. */
    GoProStatus status() const;

private:
    static void taskTrampoline(void *arg);
    void        task();

    /* Which response characteristic a notification arrived on. The payloads are
     * ambiguous without it: a command ack is [cmdId][status], and cmdId 0x02
     * would otherwise be indistinguishable from a query reply. */
    enum Chan : uint8_t { CHAN_CMD, CHAN_SET, CHAN_QUERY };

    bool connectCamera();
    void teardown();
    void onNotify(Chan chan, const uint8_t *data, size_t len);
    void parsePayload(Chan chan, const uint8_t *p, uint16_t len);
    void stashDump(uint8_t id, const uint8_t *p, uint16_t len);
    void drainDump();
    bool writeCmd(const uint8_t *data, size_t len);

    /* Multi-packet reassembly. Shared across the three response channels, so it
     * records which one owns the packet in flight — a stray interleave resets it
     * rather than splicing two replies together. */
    struct Accum {
        uint8_t  buf[512];
        uint16_t expected;
        uint16_t len;
        bool     active;
        Chan     chan;
    };
    Accum m_acc = {};

    /* Deferred hex/settings dump.
     *
     * Long replies cannot be logged from the notify callback: that runs on the
     * NimBLE host task and races loopTask for the USB-CDC buffer, which shreds
     * anything past a line or two (observed: a settings dump arriving as
     * "18=4 121=3 122=053=0"). The callback parks the payload here and the
     * GoPro task prints it in small chunks with gaps, where stalling is safe. */
    struct Dump {
        uint8_t       buf[256];
        uint16_t      len;
        uint8_t       id;       /* query/reply id the payload came from */
        volatile bool ready;
    };
    Dump m_dump = {};

    /* Keep-alive acks repeat every few seconds; log only on change. */
    uint8_t m_lastSetId     = 0xFF;
    uint8_t m_lastSetStatus = 0xFF;

    void *m_client   = nullptr;   /* NimBLEClient*             — opaque here */
    void *m_chCmd    = nullptr;   /* NimBLERemoteCharacteristic* 0x0072      */
    void *m_chSet    = nullptr;   /* NimBLERemoteCharacteristic* 0x0074      */
    void *m_chQuery  = nullptr;   /* NimBLERemoteCharacteristic* 0x0076      */

    GoProStatus m_st = { false, false, false, false, 255, 0 };

    volatile bool m_desiredRec   = false;
    uint32_t      m_lastAttemptMs = 0;
    uint32_t      m_lastKeepAliveMs = 0;
    uint32_t      m_lastShutterMs   = 0;

    char m_nameFilter[24] = {0};

    mutable portMUX_TYPE m_mux = portMUX_INITIALIZER_UNLOCKED;
};

#endif // GOPRO_MANAGER_H
