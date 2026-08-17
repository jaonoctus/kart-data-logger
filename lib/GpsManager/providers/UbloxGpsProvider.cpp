#include "UbloxGpsProvider.h"

#include <math.h>
#include <time.h>

namespace {
// UBX is little-endian throughout.
inline uint16_t rdU2(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
inline uint32_t rdU4(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline int32_t rdI4(const uint8_t *p) { return (int32_t)rdU4(p); }
}

UbloxGpsProvider::UbloxGpsProvider(int8_t rxPin, int8_t txPin)
    : _rxPin(rxPin), _txPin(txPin), _serialGps(1) {}

uint64_t UbloxGpsProvider::getEpochMs() {
    // millis() as the fallback matches what the NMEA path did, and LogManager already
    // rejects anything before 2021 when deciding whether to sync the system clock.
    return _timeValid ? _epochMs : millis();
}

bool UbloxGpsProvider::begin() {
    // 115200 first (where we leave the module), then 38400 — the u-blox M9 default,
    // and what the Matek M9N-5883 ships at. The 9600 of the NEO-6M era is gone;
    // this provider targets M9 (NEO-M9N) and nothing older.
    // ponytail: 9600/57600 are here for bring-up only — a factory M9 is 38400 and we
    // leave it at 115200. Trim back to {115200, 38400} once the wiring is trusted.
    static const uint32_t kProbeBauds[] = { 115200, 38400, 9600, 57600 };
    uint32_t foundBaud = 0;

    for (uint32_t baud : kProbeBauds) {
        log_i("Testing u-blox connection at %u baud...", baud);
        _serialGps.end();
        _serialGps.setRxBufferSize(1024);
        _serialGps.begin(baud, SERIAL_8N1, _rxPin, _txPin);

        /* Accept either sync byte. '$' is an NMEA sentence, which is what a factory
         * module sends; 0xB5 is the first byte of a UBX frame, which is all it sends
         * once configureUblox() has run and saved to flash. Probing only for '$'
         * would find nothing on the second boot and declare the module missing. */
        /* Count every byte, not just the sync ones. Discarding non-matching bytes
         * makes "module is mute" and "module talks at some other baud" produce the
         * identical "not responding" error, which is the difference between a
         * wiring fault and a config fault. */
        uint32_t seen = 0;
        uint8_t first[8];
        uint32_t startCheck = millis();
        while (millis() - startCheck < 1500) {
            if (_serialGps.available() > 0) {
                uint8_t b = _serialGps.read();
                if (seen < sizeof(first)) first[seen] = b;
                seen++;
                if (b == '$' || b == 0xB5) {
                    foundBaud = baud;
                    break;
                }
            }
        }
        /* Nothing to report when the baud works — "detected at N baud" below
         * already says so, and a warning on a healthy boot trains you to ignore
         * the alert banner. Only a baud that FAILED is worth a line. */
        if (foundBaud) break;

        if (seen == 0) {
            log_w("  %u baud: 0 bytes — line is silent", baud);
        } else {
            char hex[3 * sizeof(first) + 1] = {0};
            uint32_t n = seen < sizeof(first) ? seen : sizeof(first);
            for (uint32_t i = 0; i < n; i++) sprintf(hex + 3 * i, "%02X ", first[i]);
            log_w("  %u baud: %u bytes, first: %s", baud, seen, hex);
        }
    }

    if (!foundBaud) {
        LOG_ERROR("Error: u-blox module not responding at any probed baud.");
        return false;
    }
    log_i("u-blox module detected at %u baud.", foundBaud);

    /* No setRxBufferSize() here: it is only honoured BEFORE begin(), and calling
     * it on an open port logs an error every boot. The 1024 set in the probe
     * loop above is ~11 NAV-PVT frames (~440ms at 25Hz) and measured sufficient —
     * a 293s session logged 7329 rows with every gap exactly 40ms. If loop() ever
     * gets slow enough to overflow it, raise the value at the begin() sites. */

    if (foundBaud != 115200) {
        // GGA+RMC at 10Hz would fit in 38400, but 115200 is free and leaves room to
        // raise the rate (25Hz is ~36kbaud) without another round trip through here.
        log_i("Upgrading u-blox port to 115200...");

        const uint8_t setBaud115200[] = {
            0x01,                   // Port ID (UART1)
            0x00,                   // Reserved
            0x00, 0x00,             // Reserved
            0xD0, 0x08, 0x00, 0x00, // UART mode (8N1)
            0x00, 0xC2, 0x01, 0x00, // Baud rate (115200)
            0x03, 0x00,             // InProtoMask (UBX+NMEA)
            0x03, 0x00,             // OutProtoMask (UBX+NMEA)
            0x00, 0x00,             // Reserved
            0x00, 0x00              // Reserved
        };
        sendUBXWithChecksum(0x06, 0x00, (uint8_t*)setBaud115200, sizeof(setBaud115200));
        _serialGps.flush();
        delay(200);

        _serialGps.end();
        _serialGps.setRxBufferSize(1024);
        _serialGps.begin(115200, SERIAL_8N1, _rxPin, _txPin);
    }

    log_i("u-blox serial communication established.");

    /* log_i, not log_d: the build runs at CORE_DEBUG_LEVEL=INFO (see platformio.ini —
     * DEBUG makes the dash boot dark over USB), so log_d compiles to nothing. These
     * three lines are the only evidence that anything we send actually reaches the
     * module, and they fire once at boot rather than per fix. */
    UBXConfig initial = readCurrentConfig();
    log_i("u-blox BEFORE: Model: %d | Rate: %dms | PerfMode: %d", initial.dynModel, initial.measRate, initial.perfMode);

    if (initial.dynModel != 4 || initial.measRate != kUpdateIntervalMs || initial.perfMode != 0) {
        log_i("u-blox config is not ideal for kart. Applying adjustments...");
        configureUblox();

        delay(200);
        UBXConfig applied = readCurrentConfig();
        log_i("u-blox AFTER:  Model: %d | Rate: %dms | PerfMode: %d", applied.dynModel, applied.measRate, applied.perfMode);
    } else {
        log_i("u-blox configuration already optimized for kart.");
    }

    return true;
}

void UbloxGpsProvider::end() {
    _serialGps.end();
}

/* Software backup mode via UBX-RXM-PMREQ. This is an actual power-down of the
 * receiver core, not the ATGM336's "talk less and hope" — the whole reason charge
 * mode is worth anything on this module.
 *
 * duration 0 means indefinite, so the only way out is a wakeup source. We arm
 * uartrx, since the ESP32's TX to the module is already wired and the UART is
 * deliberately left open by enterChargeMode() for exactly this.
 *
 * What survives: the port settings, rate, dynamic model and message set, because
 * configureUblox() ends with a CFG-CFG save to flash. What does not necessarily
 * survive is the BBR ephemeris — whether it does depends on V_BCKP on the module,
 * which we cannot see from here. So wake() may come back to a hot start (~2s) or a
 * cold one (~30s). Either is correct; only the first fix is slower.
 *
 * Not called on the way into a session, only from charge mode. */
void UbloxGpsProvider::standby() {
    const uint8_t pmreq[] = {
        0x00,                   // version
        0x00, 0x00, 0x00,       // reserved1
        0x00, 0x00, 0x00, 0x00, // duration: 0 = until a wakeup source fires
        0x06, 0x00, 0x00, 0x00, // flags: backup (bit 1) | force (bit 2)
        0x08, 0x00, 0x00, 0x00  // wakeupSources: uartrx (bit 3)
    };
    sendUBXWithChecksum(0x02, 0x41, (uint8_t*)pmreq, sizeof(pmreq));
    _serialGps.flush();
    log_i("u-blox: software backup requested (UBX-RXM-PMREQ).");
}

void UbloxGpsProvider::wake() {
    /* Any traffic on the module's RX pin ends backup. The first bytes are eaten
     * waking the core rather than parsed, so this is deliberately junk and
     * deliberately more than one — 0xFF cannot begin a UBX frame or an NMEA
     * sentence, so a byte that does land in the parser is discarded rather than
     * mistaken for a command. */
    for (uint8_t i = 0; i < 8; i++) _serialGps.write(0xFF);
    _serialGps.flush();

    /* No reconfiguration to do here, unlike the ATGM336: the settings went to flash
     * in configureUblox(), so the receiver comes back up already correct. Only the
     * fix takes a moment. */
    log_i("u-blox: woken from software backup; waiting on the first fix.");
}

void UbloxGpsProvider::configureUblox() {
    // CFG-RATE — see kUpdateIntervalMs for why it is what it is.
    const uint8_t setRate[] = {
        (uint8_t)(kUpdateIntervalMs & 0xFF), (uint8_t)((kUpdateIntervalMs >> 8) & 0xFF), // Measurement Rate
        0x01, 0x00, // Navigation Rate (1 = output every measurement)
        0x01, 0x00, // Time Reference (1 = GPS time)
    };
    sendCfg(0x06, 0x08, (uint8_t*)setRate, sizeof(setRate), "CFG-RATE");

    /* CFG-NAV5: dynamic model = automotive. There is no kart model; automotive is the
     * closest fit and tunes the receiver's motion assumptions for road-vehicle
     * dynamics (its stated envelope is 100 m/s horizontal, 15 m/s vertical, 6000m
     * altitude — comfortably around a kart).
     *
     * The mask is 0xFFFF, i.e. apply EVERY field below, not just dynModel. That is
     * why the values that follow matter even where they look incidental. */
    const uint8_t setAutomotive[] = {
        0xFF, 0xFF,             // mask: all fields
        0x04,                   // dynModel  4 = automotive
        0x03,                   // fixMode   3 = auto 2D/3D
        /* fixedAlt / fixedAltVar, scaled 0.01m and 0.0001m^2 — so these are 81.95m
         * and 1680.34m^2, NOT the 800m and 8.33m^2 an earlier comment here claimed.
         * Both only apply to a 2D fix, which hasFix() now rejects outright, so they
         * are dead values kept only because the mask above writes the whole struct. */
        0x03, 0x20, 0x00, 0x00,
        0x45, 0x64, 0x00, 0x01,
        /* Everything below was commented "leave as is", which the 0xFFFF mask makes
         * untrue — every field here is written. They happen to equal the u-blox
         * defaults, which is why nothing broke: minElev 5deg, pDop/tDop 25.0,
         * pAcc 100m, tAcc 300m, staticHoldThresh 0, dgnssTimeout 60s. */
        0x05,                   // minElev, degrees
        0x00,                   // drLimit, seconds
        0xFA, 0x00,             // pDop,  0.1  -> 25.0
        0xFA, 0x00,             // tDop,  0.1  -> 25.0
        0x64, 0x00,             // pAcc,  metres -> 100
        0x2C, 0x01,             // tAcc,  metres -> 300
        0x00,                   // staticHoldThresh, cm/s
        0x3C,                   // dgnssTimeout, seconds
        /* cnoThreshNumSVs, cnoThresh, reserved1, staticHoldMaxDist, utcStandard
         * (0 = automatic) and reserved2. All zero, all matching the defaults. */
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };
    sendCfg(0x06, 0x24, (uint8_t*)setAutomotive, sizeof(setAutomotive), "CFG-NAV5 automotive");

    /* CFG-MSG: every NMEA sentence off, UBX-NAV-PVT on. NAV-PVT alone carries
     * everything the dash and the log want — position, Doppler ground speed, UTC,
     * numSV, fixType, pDOP, hAcc, sAcc — in one frame per epoch, so there is no
     * reason to keep a second, wordier copy of a subset of it on the wire.
     *
     * Turning GGA and RMC off is also what makes the four-constellation question go
     * away entirely: the per-system duplication that made GSA and GSV expensive was
     * an NMEA problem, and NAV-PVT has no talker IDs to multiply. */
    uint8_t nmeaKillList[] = {
        0x00, // GGA
        0x01, // GLL
        0x02, // GSA
        0x03, // GSV
        0x04, // RMC
        0x05, // VTG
        0x08, // ZDA
        0x0A, // DTM
        0x09, // GBS
        0x06, // GRS
        0x07, // GST
        0x41  // TXT
    };

    for (uint8_t id : nmeaKillList) {
        uint8_t payload[3] = { 0xF0, id, 0x00 }; // Class 0xF0 (NMEA), ID, Rate 0
        sendUBXWithChecksum(0x06, 0x01, payload, 3);
        delay(100);
    }

    // Class 0x01 (NAV), ID 0x07 (PVT), one per navigation solution.
    uint8_t enablePvt[3] = { 0x01, 0x07, 0x01 };
    sendCfg(0x06, 0x01, enablePvt, 3, "CFG-MSG NAV-PVT on");

    const uint8_t setMaxPerf[] = {
        0x08,
        0x00,
    };
    sendCfg(0x06, 0x11, (uint8_t*)setMaxPerf, sizeof(setMaxPerf), "CFG-RXM max performance");

    const uint8_t saveConfigAll[] = {
        0x00, 0x00, 0x00, 0x00,
        0x1F, 0x1F, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x17,
    };
    sendCfg(0x06, 0x09, (uint8_t*)saveConfigAll, sizeof(saveConfigAll), "CFG-CFG save to flash");
}

/* Walks the byte stream for an ACK-ACK (0x05/0x01) or ACK-NAK (0x05/0x00) whose
 * 2-byte payload names the class/id we just sent. Other frames — NAV-PVT included
 * — are skipped by the state machine rather than buffered. */
bool UbloxGpsProvider::awaitAck(uint8_t msgClass, uint8_t msgID, uint32_t timeoutMs) {
    uint32_t deadline = millis() + timeoutMs;
    uint8_t state = 0, ackType = 0, cls = 0;

    while ((int32_t)(deadline - millis()) > 0) {
        if (!_serialGps.available()) { delay(1); continue; }
        uint8_t b = _serialGps.read();

        switch (state) {
            case 0: state = (b == 0xB5) ? 1 : 0; break;
            case 1: state = (b == 0x62) ? 2 : 0; break;
            case 2: state = (b == 0x05) ? 3 : 0; break;   /* class ACK */
            case 3: ackType = b; state = 4;      break;   /* 0x01 ACK / 0x00 NAK */
            case 4: state = (b == 0x02) ? 5 : 0; break;   /* length lo = 2 */
            case 5: state = (b == 0x00) ? 6 : 0; break;   /* length hi = 0 */
            case 6: cls = b; state = 7;          break;
            case 7:
                state = 0;
                if (cls == msgClass && b == msgID) return ackType == 0x01;
                break;
        }
    }
    return false;   /* timeout — reported by the caller, which knows the name */
}

void UbloxGpsProvider::sendCfg(uint8_t msgClass, uint8_t msgID, uint8_t* payload,
                               uint16_t len, const char* what) {
    sendUBXWithChecksum(msgClass, msgID, payload, len);
    if (awaitAck(msgClass, msgID, 1200)) {
        log_i("u-blox cfg OK: %s", what);
    } else {
        /* NAK or silence. Either way the receiver is not running what we think
         * it is, which is worth an error rather than a shrug. */
        LOG_ERROR_FORMATTED("u-blox cfg REJECTED/no ACK: %s (0x%02X/0x%02X)",
                            what, msgClass, msgID);
    }
}

void UbloxGpsProvider::sendUBXWithChecksum(uint8_t msgClass, uint8_t msgID, uint8_t* payload, uint16_t len) {
    uint8_t header[] = { 0xB5, 0x62, msgClass, msgID, (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF) };
    uint8_t ckA = 0;
    uint8_t ckB = 0;

    _serialGps.write(header, 6);
    for (int i = 2; i < 6; i++) {
        ckA += header[i];
        ckB += ckA;
    }

    for (int i = 0; i < len; i++) {
        _serialGps.write(payload[i]);
        ckA += payload[i];
        ckB += ckA;
    }

    _serialGps.write(ckA);
    _serialGps.write(ckB);
}

bool UbloxGpsProvider::pollUBX(uint8_t msgClass, uint8_t msgID, uint8_t* payload, uint8_t payloadLen) {
    uint8_t request[] = { 0xB5, 0x62, msgClass, msgID, 0x00, 0x00, 0x00, 0x00 };

    uint8_t ckA = 0;
    uint8_t ckB = 0;
    for (uint8_t i = 2; i < 6; i++) {
        ckA += request[i];
        ckB += ckA;
    }
    request[6] = ckA;
    request[7] = ckB;

    _serialGps.write(request, sizeof(request));

    uint32_t start = millis();
    int pos = 0;

    while (millis() - start < 1000) {
        if (!_serialGps.available()) {
            continue;
        }

        uint8_t b = _serialGps.read();
        if (pos == 0 && b == 0xB5) pos++;
        else if (pos == 1 && b == 0x62) pos++;
        else if (pos == 2 && b == msgClass) pos++;
        else if (pos == 3 && b == msgID) pos++;
        else if (pos >= 4) {
            // pos 4/5 are the length bytes; the payload starts at pos 6. Without
            // the pos >= 6 guard these two write payload[-2] and payload[-1].
            if (pos >= 6 && (pos - 6) < payloadLen) {
                payload[pos - 6] = b;
            }
            pos++;
            if (pos >= (payloadLen + 8)) {
                return true;
            }
        } else {
            pos = 0;
        }
    }
    return false;
}

UBXConfig UbloxGpsProvider::readCurrentConfig() {
    UBXConfig cfg = {0, 0, 0};
    uint8_t buffer[64];

    if (pollUBX(0x06, 0x24, buffer, 36)) {
        cfg.dynModel = buffer[2];
    }

    if (pollUBX(0x06, 0x08, buffer, 6)) {
        cfg.measRate = (buffer[1] << 8) | buffer[0];
    }

    if (pollUBX(0x06, 0x11, buffer, 2)) {
        cfg.perfMode = buffer[1];
    }

    return cfg;
}

/* Byte-at-a-time UBX reader. Returns true once a NAV-PVT frame has been received
 * whole and its checksum verified, which is what the main loop treats as "there is a
 * new fix". A bad checksum drops the frame silently rather than feeding a corrupt
 * position into lap timing; the counter exists so a wiring or baud problem shows up
 * as something other than a dash that simply never updates. */
bool UbloxGpsProvider::update() {
    bool newData = false;

    while (_serialGps.available() > 0) {
        uint8_t b = _serialGps.read();

        switch (_rxState) {
        case 0: // waiting for sync char 1
            if (b == 0xB5) _rxState = 1;
            break;
        case 1: // sync char 2
            _rxState = (b == 0x62) ? 2 : 0;
            break;
        case 2: // class — the checksum covers everything from here to the payload end
            _rxClass = b; _ckA = b; _ckB = b;
            _rxState = 3;
            break;
        case 3: // id
            _rxId = b; _ckA += b; _ckB += _ckA;
            _rxState = 4;
            break;
        case 4: // length low
            _rxLen = b; _ckA += b; _ckB += _ckA;
            _rxState = 5;
            break;
        case 5: // length high
            _rxLen |= (uint16_t)b << 8; _ckA += b; _ckB += _ckA;
            _rxPos = 0;
            if (_rxLen > kRxBufSize) _rxState = 0;   // not ours; resync
            else if (_rxLen == 0)    _rxState = 7;
            else                     _rxState = 6;
            break;
        case 6: // payload
            _rxBuf[_rxPos++] = b; _ckA += b; _ckB += _ckA;
            if (_rxPos >= _rxLen) _rxState = 7;
            break;
        case 7: // checksum A
            _rxCkA = b;
            _rxState = 8;
            break;
        case 8: // checksum B
            if (_rxCkA == _ckA && b == _ckB) {
                if (_rxClass == 0x01 && _rxId == 0x07 && _rxLen >= 92) {
                    applyNavPvt(_rxBuf);
                    _pvtFrames++;
                    /* Return on EVERY completed frame instead of draining the
                     * buffer and keeping only the last. This object holds one
                     * fix, so a caller that polls slower than 25Hz used to lose
                     * every frame but the newest — silently, upstream of the log
                     * queue, which is why the health counter never saw it.
                     * Callers now loop on update() and get all of them; the
                     * unread bytes simply stay in the UART buffer. */
                    _rxState = 0;
                    return true;
                }
            } else if ((++_checksumErrors % 64) == 1) {
                /* Rate-limited: a baud or wiring fault produces these by the
                 * thousand, and the point is to be visible without shredding the
                 * serial log the way an unfiltered warning at 25Hz would. */
                log_w("u-blox: %lu UBX checksum failures", (unsigned long)_checksumErrors);
            }
            _rxState = 0;
            break;
        }
    }

    return newData;
}

/* UBX-NAV-PVT, version 0, 92 bytes. Offsets are from the interface description and
 * are the reason this reads as a pile of magic numbers — the alternative is a packed
 * struct, which needs the compiler to agree about padding on a field layout that
 * mixes U1/U2/U4 at unaligned offsets. Explicit offsets cannot be quietly wrong. */
void UbloxGpsProvider::applyNavPvt(const uint8_t *p) {
    _fixType   = p[20];
    _gnssFixOK = (p[21] & 0x01) != 0;
    _numSV     = p[23];
    _lng       = rdI4(p + 24) * 1e-7;   // 1e-7 degrees == ~1.1cm, no NMEA rounding
    _lat       = rdI4(p + 28) * 1e-7;
    _hAccM     = rdU4(p + 40) / 1000.0f;
    _gSpeedKmh = rdI4(p + 60) * 0.0036; // mm/s -> km/h; Doppler, not position delta
    _sAccMps   = rdU4(p + 68) / 1000.0f;
    _pdop      = rdU2(p + 76) * 0.01f;

    // valid: bit0 validDate, bit1 validTime, bit2 fullyResolved.
    if ((p[11] & 0x03) == 0x03) {
        struct tm t = {};
        t.tm_year  = rdU2(p + 4) - 1900;
        t.tm_mon   = p[6] - 1;
        t.tm_mday  = p[7];
        t.tm_hour  = p[8];
        t.tm_min   = p[9];
        t.tm_sec   = p[10];
        t.tm_isdst = 0;

        /* nano is signed and can run slightly negative, so this is computed in int64
         * before it becomes an unsigned epoch — the same sum in uint64 would wrap. */
        int64_t ms = (int64_t)mktime(&t) * 1000LL + (int64_t)rdI4(p + 16) / 1000000LL;
        if (ms > 0) {
            _epochMs   = (uint64_t)ms;
            _timeValid = true;
        }
    }
}

double UbloxGpsProvider::getLat() { return _lat; }
double UbloxGpsProvider::getLng() { return _lng; }

GpsFixInfo UbloxGpsProvider::getFixInfo() const {
    return { _fixType, _gnssFixOK, _pdop, _hAccM, _sAccMps };
}

double UbloxGpsProvider::getSpeed(float gForce, float gyroZ) {
    double raw = _gSpeedKmh;

    if (!hasFix()) {
        return 0.0;
    }

    _speedFiltered = _speedAlpha * raw + (1.0 - _speedAlpha) * _speedFiltered;

    if (_speedFiltered >= _minSpeedToMove) {
        if (_moveCounter < _moveCountThreshold) {
            _moveCounter++;
        }
    } else if (_speedFiltered <= _minSpeedToStop) {
        _moveCounter = 0;
    }

    bool gpsMoving = (_moveCounter >= _moveCountThreshold);
#if defined(ENABLE_IMU)
    bool imuActive = gForce > _imuDynGStop || fabsf(gyroZ) > _imuGyroZStop;
#else
    (void)gForce;
    (void)gyroZ;
    bool imuActive = false;
#endif
    bool consideredMoving = gpsMoving || imuActive;

    if (!consideredMoving) {
        _speedFiltered *= 0.5f;
        if (_speedFiltered < 0.5f) {
            _speedFiltered = 0.0f;
        }
        return 0.0;
    }

    // Round to 1 decimal place for clean telemetry logging
    return round(_speedFiltered * 10.0) / 10.0;
}

uint32_t UbloxGpsProvider::getSatellites() { return _numSV; }

/* The receiver's own verdict, which is better information than the sats >= 4 and
 * HDOP <= 3 heuristic this replaces: gnssFixOK is the flag the receiver sets when it
 * considers the solution usable under its own DOP and accuracy masks, and hAcc is its
 * error estimate rather than a proxy for one.
 *
 * fixType 3 (3D) is required. A 2D fix means the receiver could not resolve altitude,
 * which on an open kart track means the solution is struggling, and its horizontal
 * component is not worth timing a lap with. */
bool UbloxGpsProvider::hasFix() {
    if (!_gnssFixOK) return false;
    if (_fixType != 3) return false;
    if (_hAccM <= 0.0f || _hAccM > kMaxHAccM) return false;

    return true;
}

uint16_t UbloxGpsProvider::getUpdateIntervalMs() const {
    return kUpdateIntervalMs;
}
