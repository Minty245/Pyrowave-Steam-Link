// Steam Link Pyrowave - user settings (PC). See slp_settings.h.
// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include "slp_settings.h"
#include "../common/slp_json_min.h"

void slp_log(const char *fmt, ...);

namespace {

struct Def { const char *key; double value; };
// Built-in defaults (Mia's tuned values, 2026-11-07). Order = order in a newly written settings.json.
const Def kDefaults[] = {
#include "../common/slp_settings_defaults.inc"
};

std::mutex g_mu;
std::map<std::string, double> g_vals;
uint32_t g_gen = 0;
FILETIME g_mtime = {};
bool g_have_file = false;
std::chrono::steady_clock::time_point g_last_poll;
bool g_inited = false;

using slpjson::Reader;

bool read_file(const std::string &path, std::string *out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    out->clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && out->size() < (1u << 20)) out->append(buf, n);
    fclose(f);
    return true;
}

// v52: the backend uses narrow (ANSI) file APIs. A user folder whose name the ANSI code page cannot spell (e.g. a
// Cyrillic or CJK user name on an English Windows) would give a broken path. ansi_dir() returns the directory as
// ANSI if that is lossless, else its 8.3 short name (plain ASCII), else empty. dir must exist; ends with '\'.
std::string ansi_dir(const std::wstring &dir)
{
    auto narrow = [](const std::wstring &w, std::string *out) {
        BOOL lossy = FALSE;
        int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), -1, nullptr, 0, nullptr, &lossy);
        if (n <= 0 || lossy) return false;
        std::string s(size_t(n), '\0');
        WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), -1, &s[0], n, nullptr, &lossy);
        s.resize(size_t(n - 1));
        *out = s;
        return !lossy;
    };
    std::string out;
    if (narrow(dir, &out)) return out;
    wchar_t sh[MAX_PATH];
    DWORD n = GetShortPathNameW(dir.c_str(), sh, MAX_PATH);
    if (n && n < MAX_PATH && narrow(sh, &out)) return out;
    return std::string();
}

std::string temp_dir()
{
    static std::string dir = [] {
        wchar_t w[MAX_PATH];
        DWORD n = GetTempPathW(MAX_PATH, w);
        return (n && n < MAX_PATH) ? ansi_dir(w) : std::string();
    }();
    return dir;
}

std::string temp_path(const char *name)
{
    const std::string &d = temp_dir();
    return d.empty() ? std::string() : d + name;
}

bool write_json(const std::string &path, const std::map<std::string, double> &vals)
{
    std::string out = "{\n  \"version\": 1";
    std::string section;
    for (const Def &d : kDefaults) {
        std::string key = d.key;
        size_t dot = key.find('.');
        std::string sec = key.substr(0, dot), k = key.substr(dot + 1);
        if (sec != section) {
            out += section.empty() ? ",\n" : "\n  },\n";
            out += "  \"" + sec + "\": {\n";
            section = sec;
        } else {
            out += ",\n";
        }
        auto it = vals.find(key);
        double v = it != vals.end() ? it->second : d.value;
        char num[64];
        snprintf(num, sizeof(num), "%.6g", v);
        out += "    \"" + k + "\": " + num;
    }
    out += "\n  }\n}\n";
    std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok && MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// First run: build settings.json from the old %TEMP% files (if any) on top of the defaults.
void migrate(const std::string &json_path)
{
    std::map<std::string, double> v;
    for (const Def &d : kDefaults) v[d.key] = d.value;
    int found = 0;
    std::string txt;
    if (read_file(temp_path("slp_periph.txt"), &txt)) {
        found++;
        size_t pos = 0;
        while (pos < txt.size()) {
            size_t nl = txt.find('\n', pos);
            std::string line = txt.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = nl == std::string::npos ? txt.size() : nl + 1;
            size_t hash = line.find('#');
            if (hash != std::string::npos) line.resize(hash);
            char key[64];
            float val;
            if (sscanf(line.c_str(), " %63[a-z_0-9] = %f", key, &val) == 2) v[std::string("foveation.") + key] = val;
        }
    }
    if (read_file(temp_path("slp_weights.txt"), &txt)) {
        float w[4];
        if (sscanf(txt.c_str(), "%f %f %f %f", &w[0], &w[1], &w[2], &w[3]) == 4) {
            found++;
            v["weights.fovea"] = w[0];
            v["weights.wide"] = w[1];
            v["weights.centre_boost"] = w[2];
            v["weights.centre_radius"] = w[3];
        }
    }
    if (read_file(temp_path("slp_multiplier.txt"), &txt)) {
        double m = 0;
        if (sscanf(txt.c_str(), "%lf", &m) == 1 && m >= 0.25 && m <= 4.0) { found++; v["general.multiplier"] = m; }
    }
    // calibration data moves along
    std::string fit_new = slp_data_path("gaze_fit.txt");
    if (GetFileAttributesA(fit_new.c_str()) == INVALID_FILE_ATTRIBUTES)
        CopyFileA(temp_path("slp_gaze_fit.txt").c_str(), fit_new.c_str(), TRUE);
    bool ok = write_json(json_path, v);
    slp_log("[SET] created %s from %s%s", json_path.c_str(), found ? "the old %TEMP%\\slp_*.txt files" : "the built-in defaults",
            ok ? "" : " -- WRITE FAILED (using defaults in memory)");
    if (!ok) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_vals = v;
        g_gen++;
    }
}

void poll_locked(bool force)
{
    auto now = std::chrono::steady_clock::now();
    if (!force && now - g_last_poll < std::chrono::milliseconds(500)) return;
    g_last_poll = now;
    std::string path = slp_data_path("settings.json");
    WIN32_FILE_ATTRIBUTE_DATA fa;
    bool has = GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa) != 0;
    if (!has) {
        if (g_have_file || g_gen == 0) {
            g_have_file = false;
            std::map<std::string, double> v;
            for (const Def &d : kDefaults) v[d.key] = d.value;
            g_vals = v;
            g_gen++;
        }
        return;
    }
    if (g_have_file && CompareFileTime(&fa.ftLastWriteTime, &g_mtime) == 0) return;
    std::string txt;
    if (!read_file(path, &txt)) return;   // locked by a writer: try again next poll
    std::map<std::string, double> v;
    for (const Def &d : kDefaults) v[d.key] = d.value;
    Reader r{txt.data(), txt.data() + txt.size()};
    std::map<std::string, double> parsed;
    if (!r.value(std::string(), parsed, 0)) {
        static int bad = 0;
        if (bad++ < 5) slp_log("[SET] %s is not valid JSON; keeping the previous settings", path.c_str());
        g_mtime = fa.ftLastWriteTime;
        g_have_file = true;
        return;
    }
    for (auto &kv : parsed) v[kv.first] = kv.second;
    g_mtime = fa.ftLastWriteTime;
    g_have_file = true;
    g_vals = v;
    g_gen++;
    slp_log("[SET] settings.json loaded (change #%u)", g_gen);
}

void init_once()
{
    if (g_inited) return;
    g_inited = true;
    std::string path = slp_data_path("settings.json");
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        g_mu.unlock();
        migrate(path);
        g_mu.lock();
    }
    poll_locked(true);
}

} // namespace

const std::string &slp_data_dir()
{
    static std::string dir = [] {
        // v52: created with wide APIs, then used through an ANSI-safe spelling (see ansi_dir)
        wchar_t base[MAX_PATH];
        std::wstring w;
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, 0, base)))
            w = std::wstring(base) + L"\\SteamLinkPyrowave\\";
        else if (GetTempPathW(MAX_PATH, base))
            w = std::wstring(base) + L"SteamLinkPyrowave\\";
        CreateDirectoryW(w.c_str(), nullptr);
        CreateDirectoryW((w + L"logs").c_str(), nullptr);
        std::string d = ansi_dir(w);
        if (d.empty()) d = ".\\";   // unreachable in practice; keeps paths relative instead of broken
        return d;
    }();
    return dir;
}

std::string slp_data_path(const char *name) { return slp_data_dir() + name; }
std::string slp_temp_path(const char *name) { return temp_path(name); }

uint32_t slp_settings_generation()
{
    std::lock_guard<std::mutex> lk(g_mu);
    init_once();
    poll_locked(false);
    return g_gen;
}

double slp_setting(const char *key, double def)
{
    std::lock_guard<std::mutex> lk(g_mu);
    init_once();
    auto it = g_vals.find(key);
    return it != g_vals.end() ? it->second : def;
}

std::vector<std::pair<std::string, double>> slp_settings_section(const char *section)
{
    std::lock_guard<std::mutex> lk(g_mu);
    init_once();
    std::vector<std::pair<std::string, double>> out;
    std::string pre = std::string(section) + ".";
    for (auto &kv : g_vals)
        if (kv.first.compare(0, pre.size(), pre) == 0) out.emplace_back(kv.first.substr(pre.size()), kv.second);
    return out;
}
