#ifndef LAP_MANAGER_H
#define LAP_MANAGER_H

#include <Arduino.h>
#include "EspNowProtocol.h" // For TelemetryMsg/Data

// Define the track width by placing a point on the left and right sides
struct FinishLine {
    double leftLat;
    double leftLng;
    double rightLat;
    double rightLng;
};

/* Gate / sector indices. A sector is closed by the gate of the same index and
 * opened by the previous one, so:
 *
 *   sector 0  END -> S1     sector 1  S1 -> S2     sector 2  S2 -> END
 *
 * The finish line doubles as the END gate. */
enum {
    LAP_GATE_S1  = 0,
    LAP_GATE_S2  = 1,
    LAP_GATE_END = 2,
    LAP_GATE_COUNT = 3,
};

class LapManager {
public:
    void setFinishLine(const FinishLine& line);

    /* Optional split gates. Pass nullptr for either to leave it unset — with no
     * split gates the class behaves exactly as before and only times whole
     * laps. Call after setFinishLine(), which clears them. */
    void setSectorGates(const FinishLine *s1, const FinishLine *s2);

    /* Per-sample gate tracing. Useful when debugging a finish line live, but it
     * emits two or three formatted lines per sample near the gate — replaying a
     * 9,500-row session offline produces thousands of them over USB-CDC, which
     * is slow enough to matter. Off for offline analysis. */
    void setVerbose(bool v) { _verbose = v; }

    // Returns true if a lap was just completed
    bool processTelemetry(const TelemetryMsg& data);

    uint64_t getLastLapTime() const { return lastLapTimeMs; }
    uint64_t getPreviousLapTime() const { return previousLapTimeMs; }
    uint64_t getBestLapTime() const { return bestLapTimeMs; }

    /* Best lap as it stood *before* the lap just completed. This is what the
     * delta should be measured against: comparing against getBestLapTime()
     * would read 0.00 on every new personal best, because the best has already
     * been updated to the lap you are comparing. UINT64_MAX until a second lap
     * exists, i.e. until there is something to compare with. */
    uint64_t getPreviousBestLapTime() const { return previousBestLapTimeMs; }

    /* ---- sectors ------------------------------------------------------- */
    bool hasSectors() const { return _gateSet[LAP_GATE_S1] && _gateSet[LAP_GATE_S2]; }

    /* 0..2 while running, -1 before the first finish-line crossing. */
    int  getCurrentSector() const { return _currentSector; }

    /* This lap's split for sector s. 0 until it closes. */
    uint64_t getSectorTime(int s) const;

    /* Signed ms vs the best that sector had stood at before this lap's split —
     * same rule as the lap delta. LAP_SECTOR_NO_DELTA when there is nothing to
     * compare against yet. */
    static const int64_t LAP_SECTOR_NO_DELTA = INT64_MIN;
    int64_t  getSectorDelta(int s) const;

    uint64_t getSectorBest(int s) const;

    /* False when the sector's opening gate was missed this lap, so its split is
     * meaningless. A missed split never affects the lap time, which comes from
     * the finish line alone. */
    bool isSectorValid(int s) const;

    /* Elapsed time in the sector currently being driven, for a live readout. */
    uint64_t getRunningSplitMs(uint64_t nowEpochMs) const;

private:
    FinishLine _gate;
    int32_t _deltaLast = 0;
    bool _verbose = true;

    /* Gates, indexed by LAP_GATE_*. Index LAP_GATE_END mirrors _gate. */
    FinishLine _gates[LAP_GATE_COUNT] = {};
    bool       _gateSet[LAP_GATE_COUNT] = { false, false, false };
    uint64_t   _gateLastMs[LAP_GATE_COUNT] = { 0, 0, 0 };
    uint8_t    _gateBackwards[LAP_GATE_COUNT] = { 0, 0, 0 };
    /* Bumped by checkLineCrossing when a crossing is rejected for direction,
     * so the caller can tell "wrong way" from "missed the posts". */
    uint32_t   _backwardsHits = 0;
    bool       _gateWarned[LAP_GATE_COUNT] = { false, false, false };

    int      _currentSector = -1;
    uint64_t _sectorOpenMs  = 0;
    uint64_t _sectorMs      [LAP_GATE_COUNT] = { 0, 0, 0 };
    uint64_t _sectorBestMs  [LAP_GATE_COUNT] = { 0, 0, 0 };
    int64_t  _sectorDeltaMs [LAP_GATE_COUNT] = { 0, 0, 0 };
    bool     _sectorValid   [LAP_GATE_COUNT] = { false, false, false };
    bool     _sectorClosed  [LAP_GATE_COUNT] = { false, false, false };

    void resetSectorsForNewLap();
    void closeSectorAt(int gate, uint64_t crossMs);

    // Tracking the previous point to draw a line segment
    bool _hasLastPoint = false;
    double _lastLat;
    double _lastLng;
    uint64_t _lastTime;

    // Tracking the previous lap time
    uint64_t currentLapStartTime;
    uint64_t previousLapTimeMs;
    uint64_t lastLapTimeMs;
    uint64_t bestLapTimeMs;
    uint64_t previousBestLapTimeMs;
    
    const uint32_t MIN_LAP_TIME_MS = 15000; // Increased to 15s for karts

    bool checkLineCrossing(double Ax, double Ay, double Bx, double By, 
                           double Cx, double Cy, double Dx, double Dy, 
                           double &fraction);
};

#endif