// Steam Link Pyrowave - peripheral prefilter + inset locator (PC, D3D11).
// SPDX-License-Identifier: MIT
#pragma once

#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

// Runs on vrlink's encode thread with vrlink's immediate context, in place of CopyResource(dst, atlas).
// Returns false when the prefilter is off or unavailable; the caller then copies as before.
bool slp_periph_process(ID3D11DeviceContext *ctx, ID3D11Texture2D *atlas, ID3D11Texture2D *dst);

// Encoder tuning derived from the same settings (any thread). Nyquist frequencies in cycles/degree for the
// per-tile CSF; all 0 when the CSF correction is off.
void slp_periph_csf(unsigned tile_px, float *nyq_fovea, float *nyq_wide, float *nyq_display);

// v39: where each eye's inset lies in its wide view (the encoder turns it into a disc, v41) for the latest frame (tile fractions: centre x, centre y,
// half-width, half-height; half-width 0 = unknown). The part covered at every likely gaze time (the hide box).
void slp_periph_inset_boxes(float box[2][4]);

#include "slp_ramp.h"
