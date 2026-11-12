// Steam Link Pyrowave - live status for the settings app. See slp_status.h.
// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

#include "slp_settings.h"
#include "slp_status.h"

namespace {
std::mutex g_mu;
std::map<std::string, double> g_vals;
std::chrono::steady_clock::time_point g_win;
bool g_started = false;
uint64_t g_frames = 0, g_bytes = 0;
double g_enc_sum = 0, g_wait_sum = 0;

void flush_locked(double secs)
{
    g_vals["streaming"] = 1;
    g_vals["fps"] = g_frames / secs;
    g_vals["mbps"] = g_bytes * 8.0 / secs / 1e6;
    g_vals["encode_ms"] = g_frames ? g_enc_sum / g_frames : 0;   // total, as before
    g_vals["wait_ms"] = g_frames ? g_wait_sum / g_frames : 0;    // v40: of which waiting behind the game
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    double unix_s = (double)(u.QuadPart / 10000ULL) / 1000.0 - 11644473600.0;
    std::string out = "{";
    char num[96];
    snprintf(num, sizeof(num), "\"t\": %.3f", unix_s);
    out += num;
    for (auto &kv : g_vals) {
        snprintf(num, sizeof(num), ", \"%s\": %.4g", kv.first.c_str(), kv.second);
        out += num;
    }
    out += "}\n";
    std::string path = slp_data_path("status.json"), tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    if (ok) MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}
} // namespace

void slp_status_frame(double encode_ms, double wait_ms, size_t bytes)
{
    std::lock_guard<std::mutex> lk(g_mu);
    auto now = std::chrono::steady_clock::now();
    if (!g_started) { g_started = true; g_win = now; }
    g_frames++;
    g_bytes += bytes;
    g_enc_sum += encode_ms;
    g_wait_sum += wait_ms;
    double secs = std::chrono::duration<double>(now - g_win).count();
    if (secs < 1.0) return;
    flush_locked(secs);
    g_win = now;
    g_frames = g_bytes = 0;
    g_enc_sum = g_wait_sum = 0;
}

void slp_status_set(const char *key, double value)
{
    std::lock_guard<std::mutex> lk(g_mu);
    g_vals[key] = value;
}
