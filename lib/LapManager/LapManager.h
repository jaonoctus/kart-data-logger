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

class LapManager {
public:
    void setFinishLine(const FinishLine& line);

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

private:
    FinishLine _gate;
    int32_t _deltaLast = 0;
    bool _verbose = true;

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