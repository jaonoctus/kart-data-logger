#ifndef UBLOX_GPS_PROVIDER_H
#define UBLOX_GPS_PROVIDER_H

#include <Arduino.h>
#include "LoggingUtils.h"

#include "../IGpsProvider.h"

struct UBXConfig {
    uint8_t dynModel;
    uint16_t measRate;
    uint8_t perfMode;
};

/* u-blox M9 (NEO-M9N) over UBX binary, not NMEA.
 *
 * The receiver speaks UBX-NAV-PVT and nothing else: one 92-byte frame per epoch
 * carrying position, Doppler ground speed, UTC, satellite count, fix type, pDOP and
 * the receiver's own accuracy estimates. That is both less traffic than GGA+RMC
 * (~100 bytes against ~145) and strictly more information, and it arrives as a
 * single coherent snapshot rather than a position from one sentence stitched to a
 * speed from another.
 *
 * Consequence: TinyGPSPlus is not used here. It stays in lib_deps for the ATGM336
 * provider, and the linker drops it from the image when that provider is not the one
 * being built. */
class UbloxGpsProvider : public IGpsProvider {
public:
    // 25Hz — the NEO-M9N's ceiling with all four constellations, and the reason for
    // the module. NAV-PVT at this rate is ~20kbaud of the 115200 the port runs at.
    static constexpr uint16_t kUpdateIntervalMs = 40;

    UbloxGpsProvider(int8_t rxPin, int8_t txPin);

    bool begin() override;
    bool update() override;
    void end() override;

    /* Real sleep, unlike the ATGM336's quiesce — UBX-RXM-PMREQ software backup
     * powers the receiver core down. See the .cpp for what survives it. */
    void standby() override;
    void wake() override;

    double getLat() override;
    double getLng() override;
    double getSpeed(float gForce, float gyroZ) override;
    uint32_t getSatellites() override;
    uint64_t getEpochMs() override;
    bool hasFix() override;
    uint32_t getFrameCount() const override { return _pvtFrames; }
    GpsFixInfo getFixInfo() const override;
    uint16_t getUpdateIntervalMs() const override;

private:
    int8_t _rxPin;
    int8_t _txPin;
    HardwareSerial _serialGps;

    void sendUBXWithChecksum(uint8_t msgClass, uint8_t msgID, uint8_t* payload, uint16_t len);

    /* Every UBX-CFG message is answered with ACK-ACK or ACK-NAK. sendUBXWithChecksum
     * ignores that reply, so a setting the receiver REJECTS has always looked
     * exactly like one it accepted. sendCfg() sends and then reports the verdict. */
    bool awaitAck(uint8_t msgClass, uint8_t msgID, uint32_t timeoutMs);
    void sendCfg(uint8_t msgClass, uint8_t msgID, uint8_t* payload, uint16_t len,
                 const char* what);
    void configureUblox();
    UBXConfig readCurrentConfig();
    bool pollUBX(uint8_t msgClass, uint8_t msgID, uint8_t* payload, uint8_t payloadLen);
    void applyNavPvt(const uint8_t* p);

    /* Incremental UBX frame reader, fed a byte at a time by update(). NAV-PVT is 92
     * bytes; anything longer is a message we did not ask for, so the reader drops it
     * and resynchronises rather than growing a buffer for it. */
    static constexpr uint16_t kRxBufSize = 100;
    uint8_t _rxState = 0;
    uint8_t _rxClass = 0;
    uint8_t _rxId    = 0;
    uint16_t _rxLen  = 0;
    uint16_t _rxPos  = 0;
    uint8_t _ckA = 0, _ckB = 0, _rxCkA = 0;
    uint8_t _rxBuf[kRxBufSize];
    uint32_t _checksumErrors = 0;

    /* NAV-PVT frames accepted since boot. The configured rate is what we asked
     * for; this is what the receiver actually delivers, and the two are not the
     * same thing — an M9N cannot sustain 25Hz across four constellations, and it
     * does not refuse the request, it just runs slower. */
    uint32_t _pvtFrames = 0;

    // Latest NAV-PVT, in the units the rest of the system wants.
    double   _lat = 0.0;
    double   _lng = 0.0;
    double   _gSpeedKmh = 0.0;
    uint64_t _epochMs = 0;
    bool     _timeValid = false;
    uint8_t  _fixType = 0;
    bool     _gnssFixOK = false;
    uint8_t  _numSV = 0;
    float    _pdop = 0.0f;
    float    _hAccM = 0.0f;
    float    _sAccMps = 0.0f;

    /* Loose on purpose. The point of logging hAcc is to filter offline with the real
     * number in hand, so this only has to reject fixes bad enough that the dashboard
     * would mislead — not to enforce a quality bar. The old NMEA gate was
     * HDOP <= 3.0, which is roughly this in metres. */
    static constexpr float kMaxHAccM = 25.0f;

    double _speedFiltered = 0.0;
    const double _speedAlpha = 0.35;
    int _moveCounter = 0;

    const double _minSpeedToMove = 2.0;
    const double _minSpeedToStop = 1.0;
    const int _moveCountThreshold = 3;
    const float _imuDynGStop = 0.08f;
    const float _imuGyroZStop = 3.5f;
};

#endif
