// Pyrowave Link settings app - settings.json and status.json.
// SPDX-License-Identifier: MIT
#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace slp {

// %LOCALAPPDATA%\SteamLinkPyrowave\ on Windows (created if missing); $SLP_DATA_DIR or ./data/ elsewhere.
std::filesystem::path data_dir();

class Settings {
public:
    explicit Settings(std::filesystem::path dir);

    // Re-reads settings.json if another program changed it (not while our own edits are waiting to be written).
    // Returns true if any value changed.
    bool poll();
    double get(const std::string &key) const;
    double def(const std::string &key) const;
    // Changes a value; written to disk by flush() a moment later (debounced so dragging a slider is one write per ~0.15 s).
    void set(const std::string &key, double v);
    void reset(const std::vector<std::string> &keys);
    void reset_all();
    // Writes pending edits if they are older than delay_s (0 = now). Returns false on a write error.
    bool flush(double now_s, double delay_s = 0.15);
    bool pending() const { return dirty_; }
    const std::filesystem::path &path() const { return path_; }
    const std::string &last_error() const { return error_; }

private:
    bool load();
    bool write();
    std::filesystem::path dir_, path_;
    std::map<std::string, double> vals_, defaults_;
    std::vector<std::string> order_;
    std::filesystem::file_time_type mtime_{};
    bool have_mtime_ = false;
    bool dirty_ = false;
    double dirty_since_ = 0;
    std::string error_;
};

struct Status {
    bool live = false;          // status.json updated in the last 3 s
    bool pyrowave = false;
    double fps = 0, mbps = 0, encode_ms = 0, wait_ms = -1;   // wait_ms: backend v40+, -1 = not reported
    int width = 0;
    int gaze_calibrated = -1;   // -1 unknown
    double gaze_resid_px = -1, gaze_delay_ms = -1;
    int backend = 0;
};
Status read_status(const std::filesystem::path &dir);

// vrlink writes a per-frame timing capture (Steam\logs\vrlink_aftm_<date>-<time>.csv, ~28 s of frames, microseconds)
// now and then during a session. Medians over the second half of the newest capture (vrlink's latency target settles
// during the first seconds).
struct LatencyCapture {
    bool valid = false;
    std::string when;          // "11:31" from the file name
    std::string file;
    int frames = 0;
    double total_ms = 0;       // WGP Release -> Submit Illumination ("render to photon" in vrlink's terms)
    double pc_ms = 0;          // WGP Release -> Send End (app submit, compositor, vrlink GPU, encode, send)
    double net_ms = 0;         // Send End -> Receive
    double decode_ms = 0;      // Decode Start -> Decode End (our Frame layer)
    double display_ms = 0;     // Decode End -> Submit Illumination (Frame compositor + vrlink's safety margin)
};
// Folder with Steam's logs (empty if Steam is not found). $SLP_STEAM_LOGS overrides.
std::filesystem::path steam_logs_dir();
// Re-parses only when a newer capture file appears; `cache` keeps the last result.
const LatencyCapture &read_latest_capture(const std::filesystem::path &logs_dir, LatencyCapture &cache);

double now_seconds();

} // namespace slp
