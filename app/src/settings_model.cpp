// Pyrowave Link settings app - settings.json and status.json. See settings_model.h.
// SPDX-License-Identifier: MIT
#include "settings_model.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <algorithm>
#include <set>
#include <sstream>

#include "../../pc/common/slp_json_min.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

namespace slp {

namespace {
struct Def { const char *key; double value; };
const Def kDefaults[] = {
#include "../../pc/common/slp_settings_defaults.inc"
};

bool read_text(const fs::path &p, std::string *out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

bool replace_file(const fs::path &tmp, const fs::path &dst)
{
#ifdef _WIN32
    return MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code ec;
    fs::rename(tmp, dst, ec);
    return !ec;
#endif
}
} // namespace

double now_seconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

fs::path data_dir()
{
    fs::path d;
#ifdef _WIN32
    wchar_t base[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, 0, base)))
        d = fs::path(base) / L"SteamLinkPyrowave";
    else
        d = fs::temp_directory_path() / L"SteamLinkPyrowave";
#else
    const char *env = getenv("SLP_DATA_DIR");
    d = env ? fs::path(env) : fs::current_path() / "data";
#endif
    std::error_code ec;
    fs::create_directories(d / "logs", ec);
    return d;
}

Settings::Settings(fs::path dir) : dir_(std::move(dir)), path_(dir_ / "settings.json")
{
    for (const Def &d : kDefaults) {
        defaults_[d.key] = d.value;
        order_.push_back(d.key);
    }
    vals_ = defaults_;
    if (!load()) write();   // first run (the backend also creates it on its first start)
}

bool Settings::load()
{
    std::error_code ec;
    auto mt = fs::last_write_time(path_, ec);
    if (ec) return false;
    std::string txt;
    if (!read_text(path_, &txt)) return false;
    std::map<std::string, double> parsed;
    if (!slpjson::parse(txt, parsed)) {
        error_ = "settings.json is not valid JSON; it will be rewritten on the next change";
        mtime_ = mt;
        have_mtime_ = true;
        return true;
    }
    error_.clear();
    std::map<std::string, double> v = defaults_;
    for (auto &kv : parsed)
        if (kv.first != "version") v[kv.first] = kv.second;
    vals_ = v;
    mtime_ = mt;
    have_mtime_ = true;
    return true;
}

bool Settings::poll()
{
    if (dirty_) return false;
    std::error_code ec;
    auto mt = fs::last_write_time(path_, ec);
    if (ec) {   // deleted: recreate from what we show
        if (have_mtime_) { have_mtime_ = false; write(); }
        return false;
    }
    if (have_mtime_ && mt == mtime_) return false;
    auto before = vals_;
    load();
    return before != vals_;
}

double Settings::get(const std::string &key) const
{
    auto it = vals_.find(key);
    return it != vals_.end() ? it->second : def(key);
}

double Settings::def(const std::string &key) const
{
    auto it = defaults_.find(key);
    return it != defaults_.end() ? it->second : 0.0;
}

void Settings::set(const std::string &key, double v)
{
    auto it = vals_.find(key);
    if (it != vals_.end() && std::fabs(it->second - v) < 1e-9) return;
    vals_[key] = v;
    if (!dirty_) dirty_since_ = now_seconds();
    dirty_ = true;
}

void Settings::reset(const std::vector<std::string> &keys)
{
    for (auto &k : keys) set(k, def(k));
}

void Settings::reset_all()
{
    for (auto &k : order_) set(k, def(k));
}

bool Settings::flush(double now_s, double delay_s)
{
    if (!dirty_ || now_s - dirty_since_ < delay_s) return true;
    return write();
}

bool Settings::write()
{
    // Known keys in their usual order, then anything else the file had (kept, so newer backends' keys survive).
    std::vector<std::string> sections;
    std::map<std::string, std::vector<std::pair<std::string, double>>> by_sec;
    std::set<std::string> known(order_.begin(), order_.end());
    auto add = [&](const std::string &key, double v) {
        size_t dot = key.find('.');
        std::string sec = dot == std::string::npos ? "general" : key.substr(0, dot);
        std::string k = dot == std::string::npos ? key : key.substr(dot + 1);
        if (k.find('.') != std::string::npos) return;   // deeper nesting is not ours
        if (!by_sec.count(sec)) sections.push_back(sec);
        by_sec[sec].emplace_back(k, v);
    };
    for (auto &k : order_) add(k, get(k));
    for (auto &kv : vals_)
        if (!known.count(kv.first)) add(kv.first, kv.second);

    std::string out = "{\n  \"version\": 1";
    for (auto &sec : sections) {
        out += ",\n  \"" + sec + "\": {\n";
        auto &items = by_sec[sec];
        for (size_t i = 0; i < items.size(); ++i) {
            char num[64];
            snprintf(num, sizeof(num), "%.6g", items[i].second);
            out += "    \"" + items[i].first + "\": " + num + (i + 1 < items.size() ? ",\n" : "\n");
        }
        out += "  }";
    }
    out += "\n}\n";

    std::error_code ec;
    fs::create_directories(dir_, ec);
    fs::path tmp = path_;
    tmp += ".app.tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { error_ = "cannot write " + tmp.u8string(); return false; }
        f << out;
        if (!f) { error_ = "cannot write " + tmp.u8string(); return false; }
    }
    if (!replace_file(tmp, path_)) { error_ = "cannot replace " + path_.u8string(); return false; }
    dirty_ = false;
    error_.clear();
    mtime_ = fs::last_write_time(path_, ec);
    have_mtime_ = !ec;
    return true;
}

Status read_status(const fs::path &dir)
{
    Status s;
    std::string txt;
    if (!read_text(dir / "status.json", &txt)) return s;
    std::map<std::string, double> m;
    if (!slpjson::parse(txt, m)) return s;
    auto g = [&](const char *k, double d) { auto it = m.find(k); return it != m.end() ? it->second : d; };
    double unix_now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    s.live = std::fabs(unix_now - g("t", 0)) < 3.0;
    s.pyrowave = g("streaming", 0) > 0;
    s.fps = g("fps", 0);
    s.mbps = g("mbps", 0);
    s.encode_ms = g("encode_ms", 0);
    s.wait_ms = g("wait_ms", -1);
    s.width = (int)g("width", 0);
    s.gaze_calibrated = (int)g("gaze_calibrated", -1);
    s.gaze_resid_px = g("gaze_resid_px", -1);
    s.gaze_delay_ms = g("gaze_delay_ms", -1);
    s.backend = (int)g("backend", 0);
    return s;
}

fs::path steam_logs_dir()
{
    if (const char *env = getenv("SLP_STEAM_LOGS")) return fs::path(env);
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return fs::path(buf) / L"logs";
    return fs::path(L"C:\\Program Files (x86)\\Steam\\logs");
#else
    return fs::path();
#endif
}

namespace {
double median(std::vector<double> v)
{
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

bool parse_capture(const fs::path &p, LatencyCapture *out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::string line;
    if (!std::getline(f, line)) return false;
    auto split = [](const std::string &l) {
        std::vector<std::string> c;
        size_t a = 0;
        for (;;) {
            size_t b = l.find(',', a);
            std::string x = l.substr(a, b == std::string::npos ? std::string::npos : b - a);
            while (!x.empty() && (x.back() == '\r' || x.back() == ' ')) x.pop_back();
            c.push_back(x);
            if (b == std::string::npos) break;
            a = b + 1;
        }
        return c;
    };
    std::vector<std::string> head = split(line);
    auto col = [&](const char *name) {
        for (size_t i = 0; i < head.size(); ++i)
            if (head[i] == name) return (int)i;
        return -1;
    };
    const int c_wgp = col("WGP Release"), c_send_end = col("Send End"), c_recv = col("Receive"),
              c_dec0 = col("Decode Start"), c_dec1 = col("Decode End"), c_ill = col("Submit Illumination");
    if (c_wgp < 0 || c_send_end < 0 || c_recv < 0 || c_dec0 < 0 || c_dec1 < 0 || c_ill < 0) return false;
    const int need = std::max({c_wgp, c_send_end, c_recv, c_dec0, c_dec1, c_ill});
    struct R { double wgp, send_end, recv, dec0, dec1, ill; };
    std::vector<R> rows;
    while (std::getline(f, line) && rows.size() < 200000) {
        std::vector<std::string> c = split(line);
        if ((int)c.size() <= need) continue;
        R r{atof(c[c_wgp].c_str()), atof(c[c_send_end].c_str()), atof(c[c_recv].c_str()), atof(c[c_dec0].c_str()),
            atof(c[c_dec1].c_str()), atof(c[c_ill].c_str())};
        // frames that were decoded and shown, with sane ordering (rows for dropped frames carry zeros)
        if (r.wgp <= 0 || r.recv <= 0 || r.dec1 <= 0 || r.ill <= 0) continue;
        if (!(r.wgp <= r.send_end && r.send_end <= r.recv && r.dec0 <= r.dec1 && r.dec1 <= r.ill)) continue;
        if (r.ill - r.wgp > 500000) continue;
        rows.push_back(r);
    }
    if (rows.size() < 60) return false;
    std::vector<double> tot, pc, net, dec, disp;
    for (size_t i = rows.size() / 2; i < rows.size(); ++i) {
        const R &r = rows[i];
        tot.push_back((r.ill - r.wgp) / 1000.0);
        pc.push_back((r.send_end - r.wgp) / 1000.0);
        net.push_back((r.recv - r.send_end) / 1000.0);
        dec.push_back((r.dec1 - r.dec0) / 1000.0);
        disp.push_back((r.ill - r.dec1) / 1000.0);
    }
    out->frames = (int)tot.size();
    out->total_ms = median(tot);
    out->pc_ms = median(pc);
    out->net_ms = median(net);
    out->decode_ms = median(dec);
    out->display_ms = median(disp);
    out->valid = true;
    return true;
}
} // namespace

const LatencyCapture &read_latest_capture(const fs::path &logs_dir, LatencyCapture &cache)
{
    std::error_code ec;
    if (logs_dir.empty() || !fs::is_directory(logs_dir, ec)) return cache;
    std::string newest;
    for (auto it = fs::directory_iterator(logs_dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::string n = it->path().filename().u8string();
        // vrlink_aftm_YYYY-MM-DD-HH-MM-SS.csv: the name sorts by time
        if (n.size() == 35 && n.compare(0, 12, "vrlink_aftm_") == 0 && n.compare(31, 4, ".csv") == 0 && n > newest) newest = n;
    }
    if (newest.empty() || newest == cache.file) return cache;
    LatencyCapture c;
    c.file = newest;
    c.when = newest.substr(23, 2) + ":" + newest.substr(26, 2);
    if (!parse_capture(logs_dir / newest, &c)) c.valid = false;
    // a capture that cannot be read (still being written, too short) keeps the previous result on screen
    if (c.valid || !cache.valid) cache = c;
    else cache.file = newest;
    return cache;
}

} // namespace slp
