// Steam Link Pyrowave - the Frame's gaze, read from SteamVR (PC, inside vrserver).
// SPDX-License-Identifier: MIT
//
// driver_vrlink reports the Frame's eye tracking to SteamVR through IVRDriverInput_005::UpdateEyeTrackingComponent
// (vtable slot 10; slot 9 is CreateEyeTrackingComponent with "/eyetracking", from driver_vrlink.dll static analysis).
// Our SteamVR driver shim passes us the same interface object; we patch that slot (the vtable belongs to vrserver
// and is shared by every caller), record each gaze sample with a timestamp and forward the call unchanged.
//
// Data (first 28 bytes are the public VREyeTrackingData_t; vrlink's _005 struct is 40 bytes):
//   +0 bActive, +1 bValid, +2 bTracked, +4 vGazeOrigin (3 floats, vrlink sends 0), +16 vGazeTarget (gaze direction,
//   z = -1 forward), +28/+32 eye openness L/R, +36 flag.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "slp_gaze.h"

void slp_log(const char *fmt, ...);

namespace {

using UpdateEye_t = int (*)(void *self, uint64_t handle, const void *data, double time_offset);
UpdateEye_t g_orig = nullptr;
std::atomic<bool> g_hooked{false};

constexpr int kHist = 256;
std::mutex g_mu;
SlpGaze g_hist[kHist];
uint64_t g_count = 0;
std::atomic<uint64_t> g_calls{0}, g_valid_calls{0};

double qpc_now()
{
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

int hook_update_eye(void *self, uint64_t handle, const void *data, double time_offset)
{
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (data) {
        const uint8_t *d = static_cast<const uint8_t *>(data);
        SlpGaze g;
        g.active = d[0] != 0;
        g.valid = d[1] != 0;
        g.tracked = d[2] != 0;
        memcpy(&g.x, d + 16, 4);
        memcpy(&g.y, d + 20, 4);
        memcpy(&g.z, d + 24, 4);
        g.t = qpc_now() + time_offset;
        if (g.valid && std::isfinite(g.x) && std::isfinite(g.y) && std::isfinite(g.z) && g.z < -1e-3f) {
            g_valid_calls.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lk(g_mu);
            g_hist[g_count % kHist] = g;
            g_count++;
        }
    }
    return g_orig ? g_orig(self, handle, data, time_offset) : 0;
}

bool in_module(const void *p, HMODULE mod)
{
    MEMORY_BASIC_INFORMATION mbi;
    return p && VirtualQuery(p, &mbi, sizeof(mbi)) && mbi.AllocationBase == (void *)mod;
}

} // namespace

extern "C" __declspec(dllexport) void slp_set_driver_input(void *iface)
{
    if (!iface || g_hooked.load()) return;
    void **vtbl = *reinterpret_cast<void ***>(iface);
    // The interface lives in vrserver.exe (or a vrserver DLL); only patch when slots 9 and 10 point into the same
    // module as the vtable itself.
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(vtbl, &mbi, sizeof(mbi))) { slp_log("[GAZE] cannot query the vtable"); return; }
    HMODULE owner = (HMODULE)mbi.AllocationBase;
    if (!in_module(vtbl[9], owner) || !in_module(vtbl[10], owner)) {
        slp_log("[GAZE] IVRDriverInput_005 vtable slots 9/10 are outside its module; not hooking");
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(&vtbl[10], sizeof(void *), PAGE_READWRITE, &old)) {
        slp_log("[GAZE] VirtualProtect failed %lu", GetLastError());
        return;
    }
    g_orig = reinterpret_cast<UpdateEye_t>(vtbl[10]);
    vtbl[10] = reinterpret_cast<void *>(&hook_update_eye);
    DWORD tmp;
    VirtualProtect(&vtbl[10], sizeof(void *), old, &tmp);
    g_hooked.store(true);
    slp_log("[GAZE] hooked IVRDriverInput_005::UpdateEyeTrackingComponent (slot 10) in module %p", (void *)owner);
}

double slp_gaze_now() { return qpc_now(); }

bool slp_gaze_at(double t, SlpGaze *out)
{
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_count == 0) return false;
    uint64_t n = g_count < (uint64_t)kHist ? g_count : (uint64_t)kHist;
    // newest sample at or before t; else the oldest we have
    for (uint64_t i = 0; i < n; ++i) {
        const SlpGaze &g = g_hist[(g_count - 1 - i) % kHist];
        if (g.t <= t) { *out = g; return true; }
    }
    *out = g_hist[(g_count - n) % kHist];
    return true;
}

void slp_gaze_stats(uint64_t *calls, uint64_t *valid, bool *hooked)
{
    *calls = g_calls.load();
    *valid = g_valid_calls.load();
    *hooked = g_hooked.load();
}
