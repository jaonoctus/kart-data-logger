#include "LapManager.h"

/* How close the kart has to get before the (exact) intersection test is run.
 * Measured to the gate SEGMENT, not its centre: a 20 m gate has a 10 m
 * half-width, so a kart crossing near a post is ~10 m from the centre by
 * definition and a centre-based radius silently drops the crossing. That cost
 * a real lap on a real log. It also matters that the finish line and S2 can be
 * collinear and abutting — segment distance attributes proximity to the right
 * gate where a centre distance cannot. */
#define GATE_RADIUS_M    12.0

/* Minimum spacing between two accepted crossings of the SAME gate. */
#define GATE_COOLDOWN_MS 5000

void LapManager::setFinishLine(const FinishLine& line) {
    _gate = line;
    _gates[LAP_GATE_END] = line;
    _gateSet[LAP_GATE_END] = true;
    /* A new finish line invalidates the splits — they belong to a track. */
    _gateSet[LAP_GATE_S1] = false;
    _gateSet[LAP_GATE_S2] = false;

    _hasLastPoint = false;
    currentLapStartTime = 0;
    lastLapTimeMs = 0;
    previousLapTimeMs = 0;
    bestLapTimeMs = 0xFFFFFFFFFFFFFFFFULL;
    previousBestLapTimeMs = 0xFFFFFFFFFFFFFFFFULL;

    for (int i = 0; i < LAP_GATE_COUNT; i++) {
        _gateLastMs[i]    = 0;
        _gateBackwards[i] = 0;
        _gateWarned[i]    = false;
        _sectorBestMs[i]  = 0;
    }
    _currentSector = -1;
    resetSectorsForNewLap();
    log_i("LapManager: New Finish Line Set.");
}

void LapManager::setSectorGates(const FinishLine *s1, const FinishLine *s2) {
    if (s1) { _gates[LAP_GATE_S1] = *s1; _gateSet[LAP_GATE_S1] = true; }
    if (s2) { _gates[LAP_GATE_S2] = *s2; _gateSet[LAP_GATE_S2] = true; }
    log_i("LapManager: sector gates %s", hasSectors() ? "set" : "incomplete");
}

/* Distance from a point to the gate segment, in metres. Equirectangular around
 * the left post — over a gate a few tens of metres long the error is far below
 * GPS noise. */
static double distToGateSegment(double lat, double lng, const FinishLine &g) {
    const double kLat = 110540.0;
    const double kLng = 111320.0 * cos(g.leftLat * 0.017453292519943295);

    double bx = (g.rightLng - g.leftLng) * kLng;
    double by = (g.rightLat - g.leftLat) * kLat;
    double px = (lng - g.leftLng) * kLng;
    double py = (lat - g.leftLat) * kLat;

    double len2 = bx * bx + by * by;
    double t = (len2 > 0.0) ? ((px * bx + py * by) / len2) : 0.0;
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;

    double dx = px - t * bx;
    double dy = py - t * by;
    return sqrt(dx * dx + dy * dy);
}

void LapManager::resetSectorsForNewLap() {
    for (int i = 0; i < LAP_GATE_COUNT; i++) {
        _sectorMs[i]     = 0;
        _sectorDeltaMs[i] = LAP_SECTOR_NO_DELTA;
        _sectorValid[i]  = false;
        _sectorClosed[i] = false;
    }
}

/* Close the sector that gate `gate` terminates, if that is the one actually
 * running. If it is not, the sector's opening gate was missed and its split
 * would be nonsense, so it is left invalid rather than recorded wrong. */
void LapManager::closeSectorAt(int gate, uint64_t crossMs) {
    /* Clear the previous lap's splits at S1, not at the finish line. Crossing
     * the line and instantly losing the final sector's delta would throw away
     * the one number you most want to read as you come past the pits; this way
     * the rail holds the completed lap until you reach the first split. */
    if (gate == LAP_GATE_S1) resetSectorsForNewLap();

    if (_currentSector == gate && _sectorOpenMs && crossMs > _sectorOpenMs) {
        uint64_t split = crossMs - _sectorOpenMs;
        _sectorMs[gate]    = split;
        _sectorValid[gate] = true;
        _sectorClosed[gate] = true;

        uint64_t prevBest = _sectorBestMs[gate];
        _sectorDeltaMs[gate] = prevBest ? (int64_t)(split - prevBest)
                                        : LAP_SECTOR_NO_DELTA;
        if (!prevBest || split < prevBest) _sectorBestMs[gate] = split;
    } else if (_currentSector >= 0) {
        if (_verbose) log_d("LapManager: sector %d closed out of order — voided", gate);
    }
    /* The next sector always opens here, in order, even after a miss. */
    _currentSector = (gate + 1) % LAP_GATE_COUNT;
    _sectorOpenMs  = crossMs;
}

uint64_t LapManager::getSectorTime(int s) const {
    return (s >= 0 && s < LAP_GATE_COUNT) ? _sectorMs[s] : 0;
}
int64_t LapManager::getSectorDelta(int s) const {
    return (s >= 0 && s < LAP_GATE_COUNT) ? _sectorDeltaMs[s] : LAP_SECTOR_NO_DELTA;
}
uint64_t LapManager::getSectorBest(int s) const {
    return (s >= 0 && s < LAP_GATE_COUNT) ? _sectorBestMs[s] : 0;
}
bool LapManager::isSectorValid(int s) const {
    return (s >= 0 && s < LAP_GATE_COUNT) ? _sectorValid[s] : false;
}
uint64_t LapManager::getRunningSplitMs(uint64_t nowEpochMs) const {
    if (_currentSector < 0 || !_sectorOpenMs || nowEpochMs <= _sectorOpenMs) return 0;
    return nowEpochMs - _sectorOpenMs;
}

// Quick Haversine distance for logging purposes
double getDistance(double lat1, double lon1, double lat2, double lon2) {
    double p = 0.017453292519943295; // Math.PI / 180
    double a = 0.5 - cos((lat2 - lat1) * p)/2 + 
               cos(lat1 * p) * cos(lat2 * p) * (1 - cos((lon2 - lon1) * p))/2;
    return 12742000 * asin(sqrt(a)); // 2 * R; R = 6371 km
}

bool LapManager::processTelemetry(const TelemetryMsg& data) {
    if (!data.hasFix) return false;

    // We need at least two points to draw a line segment
    if (!_hasLastPoint) {
        _lastLat = data.lat;
        _lastLng = data.lng;
        _lastTime = data.timestamp;
        _hasLastPoint = true;
        return false;
    }

    /* Evaluate every configured gate against this travel segment. The gates are
     * disjoint spans, so at most one can be crossed by a single segment. */
    int    hitGate = -1;
    double fraction = 0.0;

    for (int g = 0; g < LAP_GATE_COUNT; g++) {
        if (!_gateSet[g]) continue;
        if (g != LAP_GATE_END && !hasSectors()) continue;

        double near = distToGateSegment(data.lat, data.lng, _gates[g]);
        if (near >= GATE_RADIUS_M) continue;
        if (_verbose) log_d("Approaching gate %d: %.1fm away", g, near);

        double f = 0.0;
        uint32_t before = _backwardsHits;
        if (checkLineCrossing(_lastLng, _lastLat, data.lng, data.lat,
                              _gates[g].leftLng, _gates[g].leftLat,
                              _gates[g].rightLng, _gates[g].rightLat, f)) {
            hitGate = g; fraction = f;
            _gateBackwards[g] = 0;
            break;
        }
        /* Repeated wrong-way rejections almost always mean the gate's left and
         * right posts are swapped, not that the kart is driving backwards —
         * and the symptom (a gate that simply never fires) points nowhere near
         * the cause, so say so out loud. */
        if (_backwardsHits > before && ++_gateBackwards[g] >= 3 && !_gateWarned[g]) {
            _gateWarned[g] = true;
            log_w("LapManager: gate %d rejected %u crossings as wrong-way — "
                  "are its left/right posts swapped?", g, _gateBackwards[g]);
        }
    }

    // Save current point for the next loop iteration BEFORE we return
    uint64_t timeA = _lastTime;
    _lastLat = data.lat;
    _lastLng = data.lng;
    _lastTime = data.timestamp;

    if (hitGate < 0) return false;

    uint64_t crossMs = timeA + (uint64_t)(fraction * (data.timestamp - timeA));

    /* Per-gate cooldown, so one pass cannot register twice. */
    if (_gateLastMs[hitGate] && crossMs > _gateLastMs[hitGate] &&
        (crossMs - _gateLastMs[hitGate]) < GATE_COOLDOWN_MS) return false;

    /* Split gates: close the sector they terminate and open the next. Never
     * touches lap timing — a missed or spurious split cannot corrupt a lap. */
    if (hitGate != LAP_GATE_END) {
        _gateLastMs[hitGate] = crossMs;
        closeSectorAt(hitGate, crossMs);
        return false;
    }

    // Cooldown: Prevent double-triggering if sitting on the start line
    {
        if (data.timestamp - currentLapStartTime > 10000) {
            _gateLastMs[hitGate] = crossMs;
            // INTERPOLATION: Calculate the exact millisecond the kart breached the line
            uint64_t crossingTimeMs = crossMs;

            /* The finish line closes the last sector before it starts a lap. */
            closeSectorAt(LAP_GATE_END, crossingTimeMs);

            if (currentLapStartTime != 0) {
                previousLapTimeMs = lastLapTimeMs; // Move current to previous before updating
                lastLapTimeMs = crossingTimeMs - currentLapStartTime;

                // Snapshot the best *before* folding this lap into it, so the
                // dashboard delta can be measured against the time you were
                // actually chasing. Comparing against bestLapTimeMs after the
                // update would read 0.00 on every new personal best.
                previousBestLapTimeMs = bestLapTimeMs;

                // Check for Best Lap
                if (lastLapTimeMs < bestLapTimeMs) {
                    bestLapTimeMs = lastLapTimeMs;
                }
            }
            
            // Set the start time of the next lap to the exact interpolated crossing time
            currentLapStartTime = crossingTimeMs; 
            return true;
        } else {
            if (_verbose) log_d("Lap crossed but in cooldown. Time since last lap: %llu ms", data.timestamp - currentLapStartTime);
        }
    }
    return false;
}

// 2D Line Segment Intersection Algorithm
bool LapManager::checkLineCrossing(double Ax, double Ay, double Bx, double By, 
                                   double Cx, double Cy, double Dx, double Dy, 
                                   double &fraction) {
    // Vector from A to B (Trajectory)
    double s1_x = Bx - Ax;
    double s1_y = By - Ay;
    
    // Vector from C to D (Finish Line Gate)
    double s2_x = Dx - Cx;
    double s2_y = Dy - Cy;

    double denom = s1_x * s2_y - s2_x * s1_y;
    if (denom == 0) return false; // Lines are parallel

    bool denomPositive = denom > 0;
    double s3_x = Ax - Cx;
    double s3_y = Ay - Cy;

    double s_num = s1_x * s3_y - s1_y * s3_x;
    
    // 1. Did the car cross the infinite line THIS exact frame? (0.0 to 1.0)
    if ((s_num < 0) == denomPositive || (s_num > denom) == denomPositive) {
        return false; // Hasn't reached the line, or already passed it
    }

    // 2. We crossed the line! But was it INSIDE the left/right gate posts?
    double t_num = s2_x * s3_y - s2_y * s3_x;
    if ((t_num < 0) == denomPositive || (t_num > denom) == denomPositive) {
        if (_verbose) log_d("Gate Missed! You crossed the line, but OUTSIDE the Left/Right posts.");
        return false;
    }

    // 3. We are inside the gate! DIRECTION CHECK
    double dotProduct = s1_x * (Cy - Dy) + s1_y * (Dx - Cx);
    if (dotProduct <= 0) {
        _backwardsHits++;   /* caller uses this to spot swapped posts */
        if (_verbose) log_d("Crossed Backwards! Left/Right points are swapped. (IGNORED FOR DESK TEST)");
        return false;
    }

    fraction = s_num / denom; // Exact millisecond percentage
    if (_verbose) log_d("VALID CROSSING! Fraction: %.3f", fraction);
    return true;
}