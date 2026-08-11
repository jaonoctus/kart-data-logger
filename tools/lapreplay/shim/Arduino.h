/*
 * Minimal Arduino.h stand-in so firmware libraries that only need fixed-width
 * types, <cmath> and the ESP log macros can be compiled and exercised on the
 * host. Deliberately tiny — if a library needs more than this, it probably
 * shouldn't be in a host test.
 *
 * Only used by tools/lapreplay. Never on the include path for a firmware build.
 */
#pragma once
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <deque>
#include <chrono>
#include <thread>

// Set to 1 by the harness's -DREPLAY_VERBOSE to surface LapManager's log_d()
// gate diagnostics ("Approaching Gate", "Gate Missed!", "VALID CROSSING").
#ifndef REPLAY_VERBOSE
#define REPLAY_VERBOSE 0
#endif

#define log_e(fmt, ...) fprintf(stderr, "[E] " fmt "\n", ##__VA_ARGS__)
#define log_w(fmt, ...) fprintf(stderr, "[W] " fmt "\n", ##__VA_ARGS__)
#define log_i(fmt, ...) fprintf(stderr, "[I] " fmt "\n", ##__VA_ARGS__)
#if REPLAY_VERBOSE
#define log_d(fmt, ...) fprintf(stderr, "[D] " fmt "\n", ##__VA_ARGS__)
#define log_v(fmt, ...) fprintf(stderr, "[V] " fmt "\n", ##__VA_ARGS__)
#else
#define log_d(fmt, ...) ((void)0)
#define log_v(fmt, ...) ((void)0)
#endif

/* A real clock, not a constant — firmware code spins on `while (millis() - t < N)`,
 * and a frozen millis() turns that into a hang rather than a test failure. */
inline unsigned long millis() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return (unsigned long)duration_cast<milliseconds>(steady_clock::now() - t0).count();
}
inline void delay(unsigned long ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
#define SERIAL_8N1 0

/*
 * Serial as a byte queue, nothing more. Enough to drive a provider's UBX frame
 * reader from a synthetic buffer on the host: feed() is what the receiver would
 * have sent, sent() is what the firmware wrote back.
 *
 * Still in the spirit of "deliberately tiny" — there is no timing, no baud rate and
 * no hardware behaviour here, only the read/write surface a parser touches.
 */
class HardwareSerial;
/* The most recently constructed port. A provider keeps its HardwareSerial private,
 * and this is how a host test reaches it without production code growing a test
 * seam — the test constructs exactly one provider, so there is no ambiguity. */
inline HardwareSerial *&hostSerial() { static HardwareSerial *s = nullptr; return s; }

class HardwareSerial {
public:
    explicit HardwareSerial(int) { hostSerial() = this; }
    void begin(unsigned long, int = 0, int = -1, int = -1) {}
    void end() {}
    void setRxBufferSize(size_t) {}
    void flush() {}

    int available() { return (int)_rx.size(); }
    int read() {
        if (_rx.empty()) return -1;
        uint8_t b = _rx.front();
        _rx.pop_front();
        return b;
    }
    size_t write(uint8_t b) { _tx.push_back(b); return 1; }
    size_t write(const uint8_t *p, size_t n) {
        for (size_t i = 0; i < n; i++) _tx.push_back(p[i]);
        return n;
    }

    // Host-test hooks.
    void feed(const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) _rx.push_back(p[i]); }
    const std::deque<uint8_t> &sent() const { return _tx; }
    void clearSent() { _tx.clear(); }

private:
    std::deque<uint8_t> _rx, _tx;
};
