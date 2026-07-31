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
