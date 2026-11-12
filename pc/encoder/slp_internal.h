// slp_internal.h — types shared between SteamFramePyrowaveBackend.cpp and
//                   slp_nvenc_hook.cpp.  Not part of the public DLL API.
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

struct SlpFrameStats {
    uint64_t encode_time_us;   // Pyrowave encode latency (microseconds)
    uint32_t frame_bytes;      // compressed frame size (bytes)
    uint32_t width;
    uint32_t height;
    uint32_t bitrate_kbps;     // effective bitrate (kbps)
    uint32_t chroma;           // 0 = 4:2:0, 1 = 4:4:4
    uint64_t frame_index;      // monotonic counter
};
