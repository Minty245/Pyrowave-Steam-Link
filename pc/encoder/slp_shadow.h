// Steam Link Pyrowave - v10 shadow encoder (PC side).
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <vector>

struct ID3D11DeviceContext;
struct ID3D11Texture2D;

// Called on vrlink's encoder thread for every real NvEncEncodePicture.
// ctx   = vrlink's immediate context (multithread-protected by vrlink).
// atlas = vrlink's R16G16B16A16_FLOAT RGB atlas (already rendered for this frame).
// Cheap: one CopyResource + fence signal, the Pyrowave work runs on a worker thread.
// nvenc_input = the P010 texture mapped for this EncodePicture (for the v11 sync check; may be null).
// completion_event: NVENC async-mode event of this picture (signalled when its substitution result exists).
// Returns true if a substitution result (frame or drop marker) will be pushed for encode_index.
bool slp_shadow_frame(ID3D11DeviceContext *ctx, ID3D11Texture2D *atlas, ID3D11Texture2D *nvenc_input, uint64_t encode_index,
                      void *completion_event = nullptr);

// Monotonic counters for the per-second timing log.
void slp_shadow_counters(uint64_t *submitted, uint64_t *dropped, uint64_t *samples);

// v17 substitution: true once the shadow encoder started in substitution mode (%TEMP%\slp_substitute).
bool slp_substitute_active();
// Waits up to timeout_ms for the SLPW frame of encode_index; false if none (caller sends a repeat frame).
bool slp_shadow_take_frame(uint64_t encode_index, std::vector<uint8_t> &out, int timeout_ms);

// v19 safety brake: vrlink signalled trouble (keyframe request, encoder reset, sharp target cut).
void slp_substitute_brake(const char *why);

// Shared helpers implemented in slp_nvenc_hook.cpp.
double slp_vrlink_avg_bps();
void slp_vrlink_amf_target_bps(uint64_t target_bps);
uint64_t slp_vrlink_dips_take();
void slp_log(const char *fmt, ...);
bool temp_flag_exists(const char *name);
