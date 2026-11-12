// Steam Link Pyrowave - the Frame's gaze, read from SteamVR (see slp_gaze.cpp).
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

struct SlpGaze
{
    bool active = false, valid = false, tracked = false;
    float x = 0, y = 0, z = -1; // gaze direction (head space as vrlink reports it), z < 0 forward
    double t = 0;               // seconds, slp_gaze_now() clock
};

double slp_gaze_now();
// Newest gaze sample at or before time t (seconds). False if none yet.
bool slp_gaze_at(double t, SlpGaze *out);
void slp_gaze_stats(uint64_t *calls, uint64_t *valid, bool *hooked);
