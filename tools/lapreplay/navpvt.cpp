/*
 * Host check for the real UBX-NAV-PVT reader in lib/GpsManager.
 *
 *   make navpvt && ./navpvt
 *
 * Exists because UbloxGpsProvider decodes a binary frame by explicit byte offset, and
 * a wrong offset does not crash — it produces plausible telemetry that is silently
 * wrong, which is the worst failure mode a datalogger has. This builds frames with
 * known values and asserts the provider reads them back.
 *
 * It drives the actual update() state machine through the shim's byte-queue serial,
 * so the offsets, the checksum and the frame reader under test are the ones that ship.
 */
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <vector>

#include "UbloxGpsProvider.h"

namespace {

void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
void put32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

struct PvtValues {
    int32_t  latE7    = -236048897;   // Granja Viana finish line, near enough
    int32_t  lngE7    = -468362266;
    uint8_t  fixType  = 3;
    uint8_t  flags    = 0x01;         // gnssFixOK
    uint8_t  numSV    = 14;
    uint32_t hAccMm   = 1234;
    int32_t  gSpeedMms = 25000;       // 25 m/s == 90 km/h
    uint32_t sAccMms  = 250;
    uint16_t pdopE2   = 137;          // 1.37
    uint16_t year     = 2026;
    uint8_t  month    = 8,  day = 11;
    uint8_t  hour     = 14, min = 30, sec = 20;
    uint8_t  valid    = 0x07;         // validDate | validTime | fullyResolved
    int32_t  nano     = 40000000;     // +40ms, one epoch at 25Hz
};

/* A whole UBX frame: sync, class/id, length, 92-byte payload, 8-bit Fletcher. */
std::vector<uint8_t> buildNavPvt(const PvtValues &v, bool corruptChecksum = false) {
    uint8_t pl[92] = {};
    put32(pl + 0,  123456789);        // iTOW, unread
    put16(pl + 4,  v.year);
    pl[6]  = v.month;
    pl[7]  = v.day;
    pl[8]  = v.hour;
    pl[9]  = v.min;
    pl[10] = v.sec;
    pl[11] = v.valid;
    put32(pl + 16, (uint32_t)v.nano);
    pl[20] = v.fixType;
    pl[21] = v.flags;
    pl[23] = v.numSV;
    put32(pl + 24, (uint32_t)v.lngE7);
    put32(pl + 28, (uint32_t)v.latE7);
    put32(pl + 40, v.hAccMm);
    put32(pl + 60, (uint32_t)v.gSpeedMms);
    put32(pl + 68, v.sAccMms);
    put16(pl + 76, v.pdopE2);

    std::vector<uint8_t> f = { 0xB5, 0x62, 0x01, 0x07, 92, 0 };
    f.insert(f.end(), pl, pl + sizeof(pl));

    uint8_t ckA = 0, ckB = 0;
    for (size_t i = 2; i < f.size(); i++) { ckA += f[i]; ckB += ckA; }
    f.push_back(corruptChecksum ? (uint8_t)(ckA ^ 0xFF) : ckA);
    f.push_back(ckB);
    return f;
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

} // namespace

int main() {
    /* The provider builds its epoch with mktime(), which is local time. Pinning TZ
     * makes this deterministic and documents that the firmware assumes UTC. */
    setenv("TZ", "UTC", 1);
    tzset();

    UbloxGpsProvider gps(6, 7);
    HardwareSerial *port = hostSerial();
    assert(port && "shim did not capture the provider's serial port");

    const PvtValues v;
    const std::vector<uint8_t> frame = buildNavPvt(v);

    // ---- a whole frame in one go ------------------------------------------------
    port->feed(frame.data(), frame.size());
    assert(gps.update() && "a complete NAV-PVT frame should report new data");

    assert(near(gps.getLat(), -23.6048897, 1e-7));
    assert(near(gps.getLng(), -46.8362266, 1e-7));
    assert(gps.getSatellites() == 14);
    assert(gps.hasFix() && "fixType 3 + gnssFixOK + hAcc 1.23m is a usable fix");

    GpsFixInfo info = gps.getFixInfo();
    assert(info.fixType == 3);
    assert(info.gnssFixOK);
    assert(near(info.pdop,    1.37,  1e-4));
    assert(near(info.hAccM,   1.234, 1e-4));
    assert(near(info.sAccMps, 0.25,  1e-4));

    struct tm t = {};
    t.tm_year = 2026 - 1900; t.tm_mon = 7; t.tm_mday = 11;
    t.tm_hour = 14; t.tm_min = 30; t.tm_sec = 20;
    assert(gps.getEpochMs() == (uint64_t)timegm(&t) * 1000ULL + 40);

    // ---- split across reads ----------------------------------------------------
    /* The real failure mode at 25Hz: a frame straddles two update() calls because the
     * UART buffer drained mid-message. The state machine has to resume, not resync. */
    UbloxGpsProvider split(6, 7);
    HardwareSerial *sp = hostSerial();
    for (size_t cut = 1; cut < frame.size(); cut += 7) {
        sp->feed(frame.data(), cut);
        bool first = split.update();
        sp->feed(frame.data() + cut, frame.size() - cut);
        bool second = split.update();
        assert(!first && second && "a frame split at any byte must still parse whole");
        assert(split.getSatellites() == 14);
    }

    // ---- a corrupt frame is dropped, not half-applied ---------------------------
    UbloxGpsProvider bad(6, 7);
    HardwareSerial *bp = hostSerial();
    const std::vector<uint8_t> broken = buildNavPvt(v, /*corruptChecksum=*/true);
    bp->feed(broken.data(), broken.size());
    assert(!bad.update() && "a bad checksum must not report new data");
    assert(!bad.hasFix() && "nothing from a corrupt frame should reach the getters");

    // ---- a 2D fix is not good enough --------------------------------------------
    PvtValues twoD; twoD.fixType = 2;
    UbloxGpsProvider flat(6, 7);
    HardwareSerial *fp = hostSerial();
    const std::vector<uint8_t> f2 = buildNavPvt(twoD);
    fp->feed(f2.data(), f2.size());
    assert(flat.update());
    assert(!flat.hasFix() && "a 2D fix must not count as a fix");

    // ---- hAcc gate ---------------------------------------------------------------
    PvtValues vague; vague.hAccMm = 40000;   // 40m, past kMaxHAccM
    UbloxGpsProvider poor(6, 7);
    HardwareSerial *pp = hostSerial();
    const std::vector<uint8_t> f3 = buildNavPvt(vague);
    pp->feed(f3.data(), f3.size());
    assert(poor.update());
    assert(!poor.hasFix() && "a 40m accuracy estimate must not count as a fix");

    // ---- Doppler speed reaches the filter ---------------------------------------
    /* getSpeed() runs an alpha filter and a move counter, so this asserts convergence
     * rather than a single sample — and that the mm/s -> km/h scaling is right. */
    UbloxGpsProvider fast(6, 7);
    HardwareSerial *xp = hostSerial();
    double speed = 0.0;
    for (int i = 0; i < 30; i++) {
        xp->feed(frame.data(), frame.size());
        fast.update();
        speed = fast.getSpeed(0.0f, 0.0f);
    }
    assert(near(speed, 90.0, 0.3) && "25000 mm/s should settle at 90 km/h");

    printf("navpvt: all checks passed\n");
    return 0;
}
