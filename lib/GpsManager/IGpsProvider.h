#ifndef I_GPS_PROVIDER_H
#define I_GPS_PROVIDER_H

#include <Arduino.h>

/* What the receiver says about its own fix, for logging and for gating.
 *
 * Defaults are the "it did not say" values, not the optimistic ones: a provider that
 * cannot report any of this leaves fixType 0 and the accuracy estimates at 0, which
 * reads downstream as unknown rather than perfect. Only the u-blox M9 fills it in;
 * the ATGM336's NMEA carries no equivalent.
 *
 * Note fixType never reports dead reckoning on a NEO-M9N — DR is NEO-M9V/ADR
 * silicon, so values 1 (DR only) and 4 (GNSS+DR) cannot occur on this hardware. */
struct GpsFixInfo {
    uint8_t fixType   = 0;      // 0 none, 2 = 2D, 3 = 3D, 5 = time only
    bool    gnssFixOK = false;  // the receiver considers the fix usable
    float   pdop      = 0.0f;
    float   hAccM     = 0.0f;   // horizontal accuracy estimate, metres
    float   sAccMps   = 0.0f;   // speed accuracy estimate, m/s
};

class IGpsProvider {
public:
    virtual ~IGpsProvider() = default;

    virtual bool begin() = 0;
    virtual bool update() = 0;
    virtual void end() = 0;

    /* Quiesce the receiver and bring it back. Optional: a provider with no
     * support keeps the defaults and stays fully powered.
     *
     * Note this is not necessarily sleep. On the CASIC part fitted here there
     * is no software power-down at all, so standby() only reduces how much work
     * the receiver does and silences its output. */
    virtual void standby() {}
    virtual void wake() {}

    virtual double getLat() = 0;
    virtual double getLng() = 0;
    virtual double getSpeed(float gForce, float gyroZ) = 0;
    virtual uint32_t getSatellites() = 0;
    virtual uint64_t getEpochMs() = 0;
    virtual bool hasFix() = 0;
    virtual GpsFixInfo getFixInfo() const { return {}; }

    /* Frames accepted since boot. Lets the caller measure the rate actually
     * delivered rather than trusting the rate we asked for. */
    virtual uint32_t getFrameCount() const { return 0; }

    virtual uint16_t getUpdateIntervalMs() const = 0;
};

#endif
