// Steam Link Pyrowave - PC-side encoder backend DLL.
// SPDX-License-Identifier: MIT
//
// Loaded into SteamVR's server process by the slp SteamVR driver (driver_slp), which calls slp_set_driver_input.
//
// Encoder flow (implemented in slp_nvenc_hook.cpp):
//   NvEncEncodePicture -> capture Steam Link's atlas -> encode with Pyrowave -> stash
//   NvEncLockBitstream -> substitute the stashed Pyrowave bitstream for NVENC's
//
// This file owns: the DLL entry point, the shared per-frame state, the stats export surface and the hook bootstrap.
// The Av1* exports are kept from the project's first prototype, which ran under an external stats observer; they
// only report the last frame's statistics.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

// ---------------------------------------------------------------------------
// Shared per-frame stats type (also used in slp_nvenc_hook.cpp).
#include "slp_internal.h"

// ---------------------------------------------------------------------------
// Forward declarations from slp_nvenc_hook.cpp
// ---------------------------------------------------------------------------

bool slp_nvenc_hook_install();   // IAT-patch NvEncEncodePicture + LockBitstream
void slp_nvenc_hook_remove();

static std::mutex          g_stats_mutex;
static SlpFrameStats       g_stats      = {};
static std::atomic<bool>   g_hook_ok    = false;

// Called from slp_nvenc_hook.cpp after each successful Pyrowave encode.
extern "C" void slp_update_stats(const SlpFrameStats *s)
{
    std::lock_guard<std::mutex> lk(g_stats_mutex);
    g_stats = *s;
}

// ---------------------------------------------------------------------------
// SFAO ABI types
//
// Reverse-engineered from SFAO observations (see design-decisions.md).
// Field names are descriptive; exact offsets matter, not names.
// ---------------------------------------------------------------------------

#pragma pack(push, 1)

// Returned by Av1ReadTrialIdentity (24 bytes).
struct Av1TrialIdentity {
    uint32_t version;    // must be 1
    uint32_t codec;      // 1=AV1, 2=HEVC, 3=Pyrowave (our value)
    uint64_t session_id; // arbitrary; we use the HMODULE of this DLL
    uint64_t reserved;
};
static_assert(sizeof(Av1TrialIdentity) == 24, "Av1TrialIdentity size mismatch");

// Returned by Av1ReadTrialStats (304 bytes).
// SFAO maps known fields by offset; unknown fields must be zeroed.
struct Av1BridgeStats {
    uint32_t version;              // 0x00  must be 1
    uint32_t codec;                // 0x04  mirror of identity.codec
    uint64_t frame_index;          // 0x08
    uint32_t width;                // 0x10
    uint32_t height;               // 0x14
    uint32_t bitrate_kbps;         // 0x18
    uint32_t frame_bytes;          // 0x1C
    uint64_t encode_time_us;       // 0x20
    uint32_t chroma;               // 0x28  0=420, 1=444
    uint8_t  _pad[304 - 0x2C];    // 0x2C  reserved / future use
};
static_assert(sizeof(Av1BridgeStats) == 304, "Av1BridgeStats size mismatch");

#pragma pack(pop)

// ---------------------------------------------------------------------------
// SFAO exports
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport)
BOOL Av1ReadTrialIdentity(Av1TrialIdentity *out, uint32_t out_size)
{
    if (!out || out_size < sizeof(Av1TrialIdentity)) return FALSE;
    memset(out, 0, out_size);
    out->version    = 1;
    out->codec      = 3; // Pyrowave
    out->session_id = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    return TRUE;
}

extern "C" __declspec(dllexport)
BOOL Av1ReadTrialStats(Av1BridgeStats *out, uint32_t out_size)
{
    if (!out || out_size < sizeof(Av1BridgeStats)) return FALSE;
    memset(out, 0, out_size);

    if (!g_hook_ok.load(std::memory_order_relaxed)) {
        // Not yet encoding; return zeroed struct with version/codec.
        out->version = 1;
        out->codec   = 3;
        return TRUE;
    }

    std::lock_guard<std::mutex> lk(g_stats_mutex);
    out->version        = 1;
    out->codec          = 3;
    out->frame_index    = g_stats.frame_index;
    out->width          = g_stats.width;
    out->height         = g_stats.height;
    out->bitrate_kbps   = g_stats.bitrate_kbps;
    out->frame_bytes    = g_stats.frame_bytes;
    out->encode_time_us = g_stats.encode_time_us;
    out->chroma         = g_stats.chroma;
    return TRUE;
}

// Older SFAO entry point; must exist and not crash.
extern "C" __declspec(dllexport)
void *Av1CreateLegacyFunctionTable()
{
    return nullptr; // We don't implement the legacy table.
}

// ---------------------------------------------------------------------------
// DLL entry point
// ---------------------------------------------------------------------------

extern "C"
BOOL WINAPI DllMain(HINSTANCE hmod, DWORD reason, LPVOID)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hmod);
        if (slp_nvenc_hook_install()) {
            g_hook_ok.store(true, std::memory_order_relaxed);
            OutputDebugStringA("[SLP] Pyrowave encoder backend loaded OK\n");
        } else {
            OutputDebugStringA("[SLP] NVENC hook install FAILED\n");
        }
        break;

    case DLL_PROCESS_DETACH:
        if (g_hook_ok.exchange(false)) {
            slp_nvenc_hook_remove();
        }
        break;
    }
    return TRUE;
}
