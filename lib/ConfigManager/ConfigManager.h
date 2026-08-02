#pragma once
#include <Arduino.h>

#define CONFIG_MAX_TRACKS 16

/* A split gate: two posts, crossed between them, exactly like the finish
 * line. Optional per track — a track with no sector gates simply times whole
 * laps, which is what every existing tracks.ini on a card does. */
struct SectorGate {
    double left_lat;
    double left_lon;
    bool   left_valid;
    double right_lat;
    double right_lon;
    bool   right_valid;

    bool usable() const { return left_valid && right_valid; }
};

struct TrackConfig {
    char   name[48];
    /* Finish line. Also serves as the END sector gate. */
    double left_lat;
    double left_lon;
    bool   left_valid;
    double right_lat;
    double right_lon;
    bool   right_valid;

    /* Intermediate split gates. S1 closes sector 1, S2 closes sector 2, and
     * the finish line above closes the third. */
    SectorGate s1;
    SectorGate s2;
};

// 0 = dark (default), 1 = light
class ConfigManager {
public:
    // SD must be initialized by LogManager before calling begin().
    bool begin();

    // Persists theme + selected_track to /config.ini
    bool save();

    // Persists all track entries to /tracks.ini
    bool saveTracks();

    // Reloads /tracks.ini from SD, discarding any unsaved in-memory edits
    bool reloadTracks();

    uint8_t getTheme() const { return _theme; }
    void    setTheme(uint8_t t) { _theme = t; }

    uint8_t getSelectedTrack() const { return _selected_track; }
    void    setSelectedTrack(uint8_t t) { _selected_track = t; }

    int              getTrackCount()  const { return _track_count; }
    const TrackConfig* getTrack(int i) const;
    void             setTrack(int i, const TrackConfig& tc);

private:
    uint8_t    _theme          = 0;
    uint8_t    _selected_track = 0;
    int        _track_count    = 0;
    TrackConfig _tracks[CONFIG_MAX_TRACKS];

    bool parseConfig(const String &text);
    bool parseTracks(const String &text);
};

extern ConfigManager configManager;
