// Steam Link Pyrowave - shared "ramp to the edge" state (no Windows headers: used by the NVENC hook too).
// SPDX-License-Identifier: MIT
#pragma once

// v45: the shared "ramp to the edge" for the latest frame, so the Frame filters and the bit weights follow the same
// gaze-centred curve as the prefilter. Positions and projections in tile units (fractions of the tile size):
// a pixel at tile position (x, y) looks along tan = ((x - bx) / ax, (y - by) / ay); the inset centre is (cx, cy).
struct SlpRampInfo
{
    bool valid = false;   // centres known (else the view centres)
    float cx[2] = {0.5f, 0.5f}, cy[2] = {0.5f, 0.5f};
    float ax[2] = {0.32f, 0.32f}, bx[2] = {0.58f, 0.42f}, ay[2] = {-0.33f, -0.33f}, by[2] = {0.40f, 0.40f};
    float ramp = 0, start = 15, end = 55;   // multiplier 1 + 2 * ramp * saturate((angle - start) / (end - start))
    float overshoot = 0;
};
SlpRampInfo slp_periph_ramp();
