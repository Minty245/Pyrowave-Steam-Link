// Pyrowave Link settings app - the UI. See ui.h.
// SPDX-License-Identifier: MIT
#include "ui.h"

#include <RmlUi/Core.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <vector>

#include "settings_model.h"

namespace slp {

namespace {

enum class Fmt { F2, Deg, Mul, Int, Ms, MsAuto, Level, Off1, Px };

struct RowDef {
    const char *key;
    const char *label;
    const char *hint;       // desktop hint
    const char *short_hint; // overlay hint (nullptr = desktop hint)
    bool toggle;
    float min, max, step;
    Fmt fmt;
};

// ---- every control, by settings.json key ----
const RowDef kRows[] = {
    {"general.multiplier", "Bitrate multiplier", "Above 1.0 can cause retransmits on Wi-Fi.", "Keep near 1.0 on Wi-Fi", false, 0.5f, 1.5f, 0.05f, Fmt::Mul},
    // v2.5: safety brake (backend v51): trims only the part of the multiplier above 1.0 after a lost frame
    {"general.safety_brake", "Safety brake", "After a lost frame, how much of the bitrate above 1.0x to drop for a moment. "
     "Never goes below vrlink's own target. 0 = off.", "Dip after a lost frame (above 1.0x only)", false, 0, 1, 0.05f, Fmt::F2},
    {"general.brake_recovery_ms", "Brake recovery", "How fast the bitrate climbs back after the safety brake.",
     "Climb back after a brake", false, 100, 2000, 50, Fmt::Ms},
    {"general.enabled", "Pyrowave streaming", "Off falls back to HEVC. Takes effect when the stream restarts.", "Off = HEVC, after a stream restart", true, 0, 1, 1, Fmt::Int},
    // v2.4: full colour (4:4:4, 10-bit to the compositor). Switches live.
    {"general.full_colour", "Full colour (4:4:4, 10-bit)",
     "Full-resolution colour: crisper coloured edges, text and fine colour detail. The same bitrate is shared with twice the colour "
     "data, so brightness detail gets a little less. Switches within a second. Warning: needs full colour enabled on the "
     "Frame (./install.sh --full-colour there), or the headset shows no picture.",
     "Warning: needs full colour enabled on the Frame, or no picture", true, 0, 1, 1, Fmt::Int},
    // v2.7 (backend v53): Steam Link's own 4:2:0 conversion steps, for the normal (not full colour) stream
    {"general.sharp_chroma", "Sharper colour",
     "Halves the colour resolution with a sharper filter (the one Steam Link uses for HEVC) instead of a plain average, so "
     "coloured edges and text stay crisper. Costs no bitrate. Not used with full colour.",
     "Crisper coloured edges (not with full colour)", true, 0, 1, 1, Fmt::Int},
    {"general.luma_adjust", "Luma adjustment",
     "Corrects the brightness of each pixel for the reduced colour resolution (as Steam Link does for HEVC), so saturated "
     "edges and thin coloured lines don't turn dark or blotchy. Costs no bitrate. Not used with full colour.",
     "No dark fringes on colour edges (not with full colour)", true, 0, 1, 1, Fmt::Int},
    {"weights.under_inset", "Under the inset", "Fewer bits for the part of the wide view the sharp inset covers. 1.0 = off.",
     nullptr, false, 0.1f, 1.0f, 0.05f, Fmt::Off1},
    {"weights.inset_shown", "Shown part of the inset",
     "How much of the inset the headset actually shows (fraction of its size). Both inset settings use it.",
     nullptr, false, 0.2f, 1.0f, 0.05f, Fmt::F2},
    {"weights.inset_edge_boost", "Inset edge", "Extra bits along the inset's edge in the wide view, where the two meet.",
     nullptr, false, 0, 2, 0.1f, Fmt::F2},
    {"weights.fovea", "Centre views", "Share of each frame's bits for the sharp gaze insets.", "Bits for the sharp insets", false, 0.25f, 3.0f, 0.05f, Fmt::F2},
    {"weights.wide", "Wide views", "Share for the wide views. Raising it costs centre sharpness.", "More costs centre sharpness", false, 0.25f, 3.0f, 0.05f, Fmt::F2},
    {"foveation.strength", "Blur strength", "1.0 matches human acuity.", "1.0 matches your eyes", false, 0, 2, 0.05f, Fmt::F2},
    {"foveation.contrast", "Contrast restore", "Keeps the periphery from feeling like tunnel vision.", "Less tunnel vision", false, 0, 3, 0.05f, Fmt::F2},
    {"foveation.edge_ramp", "Ramp to the edge", "More blur and contrast restore, and fewer bits, the further from your gaze. 1 = 3x at the edge.",
     "More of everything towards the edge", false, 0, 1, 0.05f, Fmt::F2},
    {"foveation.true_angles", "Accurate angles", "Measures the periphery in real degrees: the edges of the flat view get more blur.", nullptr, true, 0, 1, 1, Fmt::Int},
    {"foveation.chroma_lod", "Colour blur", "Extra blur on colour only, where it is least visible.", "Colour only, where least visible", false, 0, 3, 0.25f, Fmt::F2},
    // v2.7 (backend v54): the colour blur keeps each pixel's real brightness (luma adjustment)
    {"foveation.chroma_luma", "Colour blur keeps brightness",
     "Keeps each pixel's true brightness when its colour is blurred, so saturated edges and thin coloured lines in the "
     "periphery don't go dark. Works with and without full colour.", "No dark fringes from the colour blur", true, 0, 1, 1, Fmt::Int},
    {"foveation.gaze_now", "Follow current gaze", "Sharpens where you look now, not only where the headset's inset is.", nullptr, true, 0, 1, 1, Fmt::Int},
    {"foveation.locator", "Keep refining calibration", "Image matching in the background keeps the eye mapping accurate.", nullptr, true, 0, 1, 1, Fmt::Int},
    {"foveation.hide", "Blank area under the inset", "Saves bits under the high-detail inset. May peek on fast eye moves.", nullptr, true, 0, 1, 1, Fmt::Int},
    {"frame.wide_blur", "Blur periphery", "Contrast-preserving Gaussian blur of the wide view after decoding. Hides blocks at low bitrates.",
     "Hides blocks at low bitrates", false, 0, 4, 0.05f, Fmt::Px},
    {"frame.wide_blur_contrast", "Periphery contrast", "Restores edges after the blur, never past the decoded picture.",
     "Restores edges, never past the original", false, 0, 3, 0.1f, Fmt::F2},
    {"frame.temporal", "Headset stability", "Blends in the last decoded frame away from your gaze to calm codec flicker at low bitrates.",
     "Calms codec flicker", false, 0, 1, 0.05f, Fmt::F2},
    {"frame.temporal_clamp", "Headset stability clamp", "How far the last frame may differ before it is ignored (lower = fewer trails).", nullptr, false, 0.25f, 3, 0.05f, Fmt::F2},
    {"frame.sharpen", "Sharpen centre", "Contrast-adaptive, inset only. Use instead of SteamVR's CAS.", "Inset only", false, 0, 1, 0.05f, Fmt::F2},
    {"foveation.fov", "Display field of view", "Used to convert angles to pixels. 118 for the Steam Frame.", nullptr, false, 90, 140, 1, Fmt::Deg},
    {"foveation.max_lod", "Maximum blur", "Cap on how much the far periphery is blurred.", nullptr, false, 0, 6, 0.25f, Fmt::F2},
    {"foveation.gaze_delay_ms", "Gaze delay", "How old a gaze sample to use. Auto takes it from calibration.", nullptr, false, -1, 60, 1, Fmt::MsAuto},
    {"foveation.hide_margin", "Blank area margin", "Safety margin around the blanked area.", nullptr, false, 0, 1, 0.05f, Fmt::F2},
    {"app.dashboard", "Show in SteamVR dashboard", "Adds Pyrowave Link to the dashboard and starts it with SteamVR.", nullptr, true, 0, 1, 1, Fmt::Int},
};

const RowDef *row_def(const std::string &key)
{
    for (const RowDef &r : kRows)
        if (key == r.key) return &r;
    return nullptr;
}

// v2.6: settings that no longer have a control. They are held at their built-in default (= off), also when an older
// settings.json still has another value: saving bits on simple scenes (always fill the budget), the sharp zone (the
// inset covers it), the PC-side contrast overshoot and periphery stability (the headset stability does it better), and
// the headset deblock (the headset blur replaced it). v2.7: the middle boost of the insets (no visible effect; held at
// its default, the tested 2x).
const char *const kRetired[] = {"general.simple_scene_saving", "foveation.safe", "foveation.contrast_overshoot",
                                "foveation.temporal", "foveation.temporal_clamp", "frame.deblock_wide", "frame.deblock_inset",
                                "frame.deblock_coarse", "frame.deblock_bpp", "frame.deblock_mode",
                                "weights.centre_boost", "weights.centre_radius"};
// Not part of "the tune": reset-to-default and A/B leave these alone.
bool tune_key(const char *k)
{
    return strcmp(k, "general.enabled") && strcmp(k, "general.full_colour") && strcmp(k, "app.dashboard");
}

struct SectionDef {
    const char *id, *nav, *title;
};
const SectionDef kSections[] = {
    {"stream", "Stream", "Stream"},
    {"foveation", "Foveation", "Foveated streaming"},
    {"headset", "Headset", "Headset post-processing"},
    {"colour", "Colour", "Colour"},
    {"advanced", "Advanced", "Advanced"},
};

struct CardDef {
    const char *section, *title, *sub;
    std::vector<const char *> keys;
};
const std::vector<CardDef> kCards = {
    {"stream", "Bitrate",
     "Pyrowave follows the bandwidth target set in SteamVR's Steam Link settings. Raise that target for more quality; keep the "
     "multiplier near 1.0 so packet pacing stays clean.",
     {"general.multiplier", "general.safety_brake", "general.brake_recovery_ms"}},
    {"stream", "Where the bits go", "How each frame's budget is split between the sharp centre views and the wide views.",
     {"weights.fovea", "weights.wide", "weights.under_inset",
      "weights.inset_shown", "weights.inset_edge_boost"}},
    {"foveation", "Peripheral blur",
     "Detail your eyes cannot resolve away from the gaze is removed before encoding, so the bits go where you look.",
     {"foveation.strength", "foveation.contrast", "foveation.chroma_lod", "foveation.chroma_luma", "foveation.edge_ramp"}},
    {"foveation", "Eye tracking", "",   // subtitle filled from status.json
     {"foveation.gaze_now", "foveation.locator", "foveation.hide"}},
    {"headset", "Headset filters", "Run on the Frame after decoding. Sent with the stream: no headset setup needed.",
     {"frame.wide_blur", "frame.wide_blur_contrast", "frame.temporal", "frame.sharpen"}},
    {"colour", "", "", {"general.full_colour", "general.sharp_chroma", "general.luma_adjust"}},   // v2.6: no card header
    {"advanced", "SteamVR", "", {"app.dashboard"}},
    {"advanced", "Fine tuning", "Rarely needed. Reset to defaults puts everything back.",
     {"foveation.fov", "foveation.max_lod", "foveation.gaze_delay_ms", "foveation.hide_margin", "foveation.true_angles", "frame.temporal_clamp"}},
};

struct TabDef {
    const char *id, *label;
    std::vector<const char *> keys;
};
// Every tab has four rows: the overlay texture is sized once to the tallest tab (v1.2).
// v2.6: the Periphery tab is gone (its PC stability row was retired; the rest is on the other two tabs).
const std::vector<TabDef> kTabs = {
    {"foveation", "Foveation", {"foveation.strength", "foveation.contrast", "foveation.chroma_lod", "foveation.edge_ramp"}},
    {"headset", "Headset filters", {"frame.wide_blur", "frame.wide_blur_contrast", "frame.temporal", "frame.sharpen"}},
    {"stream", "Stream", {"general.multiplier", "general.safety_brake", "weights.fovea", "weights.wide"}},
};

std::string fmt(const char *f, double v)
{
    char b[64];
    snprintf(b, sizeof(b), f, v);
    return b;
}

std::string format_value(Fmt f, double v)
{
    switch (f) {
    case Fmt::F2: return fmt("%.2f", v);
    case Fmt::Deg: return fmt("%.0f\xC2\xB0", v);
    case Fmt::Mul: return fmt("%.2f\xC3\x97", v);
    case Fmt::Int: return fmt("%.0f", v);
    case Fmt::Ms: return fmt("%.0f ms", v);
    case Fmt::MsAuto: return v < 0 ? std::string("Auto") : fmt("%.0f ms", v);
    case Fmt::Level: return v <= 0 ? std::string("Off") : fmt("%.0f", v);
    case Fmt::Px: return v <= 0 ? std::string("Off") : fmt("%.2f px", v);
    case Fmt::Off1: return v >= 0.999 ? std::string("Off") : fmt("%.2f", v);
    }
    return fmt("%g", v);
}

// ---- data model types ----
struct Row {
    Rml::String key, label, hint, display;
    Rml::String btn;   // v2.6: "Default" (value differs from the default) / "Mine" (back to the value before) / ""
    bool slider = true, on = false, last = false;
    float min = 0, max = 1, step = 0.1f, value = 0, pct = 0, pos = 0;
};
struct Card {
    Rml::String title, sub;
    std::vector<Row> rows;
};
struct Item {
    Rml::String id, label;
    bool on = false;
};
struct Stat {
    Rml::String label, value, unit, note;
    bool accent = false;
};
struct Chip {
    Rml::String text;
    int kind = 0;   // 0 plain, 1 pink accent, 2 mint dot, 3 grey dot
};

} // namespace

struct App::Impl {
    Mode mode;
    Rml::Context *ctx;
    PlatformHooks hooks;
    std::filesystem::path dir;
    Settings settings;
    Status status;
    LatencyCapture capture;
    std::filesystem::path steam_logs = steam_logs_dir();
    double next_status = 0, next_poll = 0;
    bool in_update = false;
    bool visible_change = true;
    Rml::ElementDocument *doc = nullptr;
    Rml::DataModelHandle model;

    // bound variables
    Rml::String latency_line;
    Rml::String section = "stream", title, tab = "foveation", footer_title, footer_sub, error;
    bool streaming = false;
    std::vector<Item> nav, tabs;
    // v2.6: A/B against the defaults. mine: per control, the value it had before its Default button was used;
    // tune_mine: the whole tune before "Reset all to default". This session only.
    std::map<std::string, double> mine, tune_mine;
    Rml::String tune_btn, tune_note, enabled_note;
    bool enabled_on = true;
    std::vector<Card> cards;
    std::vector<Row> rows;   // overlay
    std::vector<Stat> stats;
    std::vector<Chip> chips;

    Impl(Mode m, Rml::Context *c, PlatformHooks h)
        : mode(m), ctx(c), hooks(std::move(h)), dir(data_dir()), settings(dir) {}

    Row make_row(const char *key)
    {
        Row r;
        const RowDef *d = row_def(key);
        if (!d) return r;
        r.key = d->key;
        r.label = d->label;
        r.hint = mode == Mode::Overlay && d->short_hint ? d->short_hint : d->hint;
        r.slider = !d->toggle;
        double v = settings.get(d->key);
        if (d->toggle) {
            r.on = v > 0.5;
            r.display = r.on ? "On" : "Off";
        } else {
            r.min = d->min;
            r.max = d->max;
            r.step = d->step;
            r.value = (float)std::min<double>(std::max<double>(v, d->min), d->max);
            r.pct = 100.f * (r.value - r.min) / (r.max - r.min);
            r.pos = std::round(r.pct * 10.f);   // the slider itself always runs 0..1000; snapping happens here
            r.display = format_value(d->fmt, d->fmt == Fmt::MsAuto ? v : r.value);   // shows what is applied (the backend clamps too)
        }
        if (tune_key(d->key)) {
            auto it = mine.find(d->key);
            if (!is_def(d->key)) r.btn = "Default";
            else if (it != mine.end() && std::fabs(it->second - settings.def(d->key)) > 1e-6) r.btn = "Mine";
        }
        return r;
    }

    bool is_def(const std::string &key) const { return std::fabs(settings.get(key) - settings.def(key)) < 1e-4; }

    // the tune = every control except the on/off switches for streaming, full colour and the dashboard
    std::vector<std::string> tune_keys() const
    {
        std::vector<std::string> v;
        for (const RowDef &r : kRows)
            if (tune_key(r.key)) v.push_back(r.key);
        return v;
    }

    void retire_old_keys()
    {
        for (const char *k : kRetired)
            if (!is_def(k)) settings.set(k, settings.def(k));
    }

    void rebuild()
    {
        int changed = 0;
        for (const std::string &k : tune_keys()) changed += !is_def(k);
        if (changed) {
            tune_btn = "Reset all to default";
            tune_note = changed == 1 ? "1 setting changed" : fmt("%.0f settings changed", changed);
        } else if (!tune_mine.empty()) {
            tune_btn = "Back to my tune";
            tune_note = "Defaults";
        } else {
            tune_btn = "";
            tune_note = "Default tune";
        }
        enabled_on = settings.get("general.enabled") > 0.5;
        enabled_note = enabled_on ? "On" : "Off \xC2\xB7 HEVC";

        nav.clear();
        for (const SectionDef &s : kSections) {
            nav.push_back({s.id, s.nav, section == s.id});
            if (section == s.id) title = s.title;
        }
        cards.clear();
        for (const CardDef &c : kCards) {
            if (section != c.section) continue;
            Card card{c.title, c.sub, {}};
            if (!strcmp(c.title, "Eye tracking")) card.sub = gaze_text();
            if (!strcmp(c.title, "SteamVR"))
                card.sub = "The overlay has the most-used settings and opens from the SteamVR dashboard.";
            for (const char *k : c.keys) card.rows.push_back(make_row(k));
            cards.push_back(std::move(card));
        }
        tabs.clear();
        rows.clear();
        for (const TabDef &t : kTabs) {
            tabs.push_back({t.id, t.label, tab == t.id});
            if (tab == t.id)
                for (const char *k : t.keys) rows.push_back(make_row(k));
        }
        if (!rows.empty()) rows.back().last = true;

        // stats (desktop) and chips (overlay)
        streaming = status.live;
        stats.clear();
        chips.clear();
        const char *dash = "\xE2\x80\x94";
        double hz = std::round(status.fps);
        const LatencyCapture &cap = capture;
        stats.push_back({"Total latency", cap.valid ? fmt("%.1f", cap.total_ms) : dash, cap.valid ? "ms" : "",
                         cap.valid ? "render to photon \xC2\xB7 " + cap.when : "after vrlink's next timing capture", true});
        latency_line = cap.valid ? std::string("Last vrlink timing capture, ") + cap.when +
                                       fmt(": PC %.1f ms", cap.pc_ms) + fmt(" \xC2\xB7 network %.1f", cap.net_ms) +
                                       fmt(" \xC2\xB7 Frame decode %.1f", cap.decode_ms) +
                                       fmt(" \xC2\xB7 Frame display %.1f ms", cap.display_ms)
                                 : std::string();
        stats.push_back({"Bitrate", status.live ? fmt("%.0f", status.mbps) : dash, "Mbps",
                         status.live && status.width ? fmt("%.0f px", status.width) : "Pyrowave stream", false});
        // v40 backend splits the PC time: Pyrowave's own encode, and the wait for vrlink's atlas rendering, which
        // queues behind the game on the GPU (large when the game is GPU-bound; not ours to speed up)
        if (status.live && status.wait_ms >= 0)
            stats.push_back({"Encode", fmt("%.1f", std::max(0.0, status.encode_ms - status.wait_ms)), "ms",
                             fmt("Pyrowave \xC2\xB7 +%.1f ms GPU queue", status.wait_ms), false});
        else
            stats.push_back({"Encode", status.live ? fmt("%.1f", status.encode_ms) : dash, "ms",
                             status.live ? "incl. GPU queue (game)" : "PC, high priority", false});
        stats.push_back({"Frame rate", status.live ? fmt("%.0f", hz) : dash, "Hz", "frames encoded per second", false});
        stats.push_back({"Gaze", status.live && status.gaze_calibrated == 1 ? fmt("%.1f", status.gaze_resid_px) : dash,
                         status.gaze_calibrated == 1 ? "px" : "",
                         !status.live ? "eye tracking" : status.gaze_calibrated == 1 ? "calibration error" : "calibrating", false});
        if (status.live) {
            chips.push_back({capture.valid ? fmt("%.1f ms total", capture.total_ms)
                                           : fmt("%.1f ms encode", status.wait_ms >= 0 ? std::max(0.0, status.encode_ms - status.wait_ms)
                                                                                         : status.encode_ms), 1});
            chips.push_back({fmt("%.0f Mbps", status.mbps), 2});
            chips.push_back({status.gaze_calibrated == 1 ? "Gaze calibrated" : "Gaze calibrating", status.gaze_calibrated == 1 ? 2 : 3});
            chips.push_back({fmt("%.0f px", status.width) + fmt(" \xC2\xB7 %.0f Hz", hz), 3});
        } else {
            chips.push_back({"Not streaming", 3});
        }
        footer_title = status.live ? "Streaming to Frame" : "Not streaming";
        footer_sub = status.live ? (status.backend ? fmt("Settings apply live. Backend v%.0f", status.backend) : "Settings apply live.")
                                 : "Settings are saved and apply when a stream starts.";
        error = settings.last_error();
        if (model) model.DirtyAllVariables();
        visible_change = true;
    }

    std::string gaze_text() const
    {
        if (!status.live) return "Gaze comes from SteamVR's eye tracking. Status shows while streaming.";
        if (status.gaze_calibrated != 1) return "Gaze comes from SteamVR. Calibrating: look around for a few seconds.";
        std::string s = fmt("Gaze comes from SteamVR. Calibrated \xC2\xB7 %.1f px error", status.gaze_resid_px);
        if (status.gaze_delay_ms >= 0) s += fmt(" \xC2\xB7 %.0f ms delay", status.gaze_delay_ms);
        return s + ".";
    }

    std::vector<std::string> visible_keys() const
    {
        std::vector<std::string> keys;
        if (mode == Mode::Overlay) {
            for (const TabDef &t : kTabs)
                if (tab == t.id)
                    for (const char *k : t.keys) keys.push_back(k);
        }
        return keys;
    }

    void on_slide(const std::string &key, double pos)
    {
        if (in_update) return;
        const RowDef *d = row_def(key);
        if (!d) return;
        double raw = d->min + std::min(std::max(pos, 0.0), 1000.0) / 1000.0 * (d->max - d->min);
        double v = d->min + std::round((raw - d->min) / d->step) * d->step;
        v = std::round(v * 1e4) / 1e4;
        double cur = settings.get(key);
        if (std::fabs(v - cur) < 1e-6) return;
        settings.set(key, v);
        rebuild();
    }

    void on_toggle(const std::string &key)
    {
        double now = settings.get(key) > 0.5 ? 0.0 : 1.0;
        settings.set(key, now);
        if (key == "app.dashboard" && hooks.dashboard_changed) hooks.dashboard_changed(now > 0.5);
        rebuild();
    }

    // v2.6: per-control A/B: Default stores the current value and resets; Mine puts the stored value back
    void on_rowreset(const std::string &key)
    {
        if (!row_def(key) || !tune_key(key.c_str())) return;
        if (!is_def(key)) {
            mine[key] = settings.get(key);
            settings.set(key, settings.def(key));
        } else {
            auto it = mine.find(key);
            if (it != mine.end()) settings.set(key, it->second);
        }
        if (key == "app.dashboard" && hooks.dashboard_changed) hooks.dashboard_changed(settings.get(key) > 0.5);
        rebuild();
    }

    // v2.6: the whole tune: Reset all to default (remembered) / Back to my tune
    void on_tune()
    {
        std::map<std::string, double> cur;
        for (const std::string &k : tune_keys())
            if (!is_def(k)) cur[k] = settings.get(k);
        if (!cur.empty()) {
            tune_mine = cur;
            for (auto &kv : cur) {
                mine[kv.first] = kv.second;
                settings.set(kv.first, settings.def(kv.first));
            }
        } else {
            for (auto &kv : tune_mine) settings.set(kv.first, kv.second);
        }
        rebuild();
    }

    void on_action(const std::string &a)
    {
        if (a == "logs" && hooks.open_path) hooks.open_path((dir / "logs").u8string());
        else if (a == "folder" && hooks.open_path) hooks.open_path(dir.u8string());
        else if (a == "file" && hooks.open_path) { settings.flush(now_seconds(), 0); hooks.open_path(settings.path().u8string()); }
        else if (a == "reset_all") { settings.reset_all(); rebuild(); }
        else if (a == "reset_tab") { settings.reset(visible_keys()); rebuild(); }
        else if (a == "tune") on_tune();
    }

    bool build_model()
    {
        Rml::DataModelConstructor c = ctx->CreateDataModel("app");
        if (!c) return false;
        if (auto h = c.RegisterStruct<Row>()) {
            h.RegisterMember("key", &Row::key);
            h.RegisterMember("label", &Row::label);
            h.RegisterMember("hint", &Row::hint);
            h.RegisterMember("display", &Row::display);
            h.RegisterMember("btn", &Row::btn);
            h.RegisterMember("slider", &Row::slider);
            h.RegisterMember("on", &Row::on);
            h.RegisterMember("last", &Row::last);
            h.RegisterMember("min", &Row::min);
            h.RegisterMember("max", &Row::max);
            h.RegisterMember("step", &Row::step);
            h.RegisterMember("value", &Row::value);
            h.RegisterMember("pct", &Row::pct);
            h.RegisterMember("pos", &Row::pos);
        }
        c.RegisterArray<std::vector<Row>>();
        if (auto h = c.RegisterStruct<Card>()) {
            h.RegisterMember("title", &Card::title);
            h.RegisterMember("sub", &Card::sub);
            h.RegisterMember("rows", &Card::rows);
        }
        c.RegisterArray<std::vector<Card>>();
        if (auto h = c.RegisterStruct<Item>()) {
            h.RegisterMember("id", &Item::id);
            h.RegisterMember("label", &Item::label);
            h.RegisterMember("on", &Item::on);
        }
        c.RegisterArray<std::vector<Item>>();
        if (auto h = c.RegisterStruct<Stat>()) {
            h.RegisterMember("label", &Stat::label);
            h.RegisterMember("value", &Stat::value);
            h.RegisterMember("unit", &Stat::unit);
            h.RegisterMember("note", &Stat::note);
            h.RegisterMember("accent", &Stat::accent);
        }
        c.RegisterArray<std::vector<Stat>>();
        if (auto h = c.RegisterStruct<Chip>()) {
            h.RegisterMember("text", &Chip::text);
            h.RegisterMember("kind", &Chip::kind);
        }
        c.RegisterArray<std::vector<Chip>>();

        c.Bind("section", &section);
        c.Bind("title", &title);
        c.Bind("tab", &tab);
        c.Bind("nav", &nav);
        c.Bind("tune_btn", &tune_btn);
        c.Bind("tune_note", &tune_note);
        c.Bind("enabled_on", &enabled_on);
        c.Bind("enabled_note", &enabled_note);
        c.Bind("tabs", &tabs);
        c.Bind("cards", &cards);
        c.Bind("rows", &rows);
        c.Bind("stats", &stats);
        c.Bind("chips", &chips);
        c.Bind("streaming", &streaming);
        c.Bind("footer_title", &footer_title);
        c.Bind("footer_sub", &footer_sub);
        c.Bind("error", &error);
        c.Bind("latency_line", &latency_line);

        c.BindEventCallback("slide", [this](Rml::DataModelHandle, Rml::Event &ev, const Rml::VariantList &args) {
            if (args.size() < 1) return;
            // Only the slider the user is working (focused) counts: RmlUi also sends "change" when a slider
            // re-clamps itself during layout.
            bool user = false;
            for (Rml::Element *f = ctx->GetFocusElement(); f; f = f->GetParentNode())
                if (f == ev.GetTargetElement()) { user = true; break; }
            if (!user) return;
            double v = ev.GetParameter<float>("value", NAN);
            if (args.size() >= 2) v = args[1].Get<float>(v);
            if (std::isfinite(v)) on_slide(args[0].Get<Rml::String>(), v);
        });
        c.BindEventCallback("toggle", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &args) {
            if (!args.empty()) on_toggle(args[0].Get<Rml::String>());
        });
        c.BindEventCallback("rowreset", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &args) {
            if (!args.empty()) on_rowreset(args[0].Get<Rml::String>());
        });
        c.BindEventCallback("section", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &args) {
            if (!args.empty()) { section = args[0].Get<Rml::String>(); rebuild(); }
        });
        c.BindEventCallback("tab", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &args) {
            if (!args.empty()) { tab = args[0].Get<Rml::String>(); rebuild(); }
        });
        c.BindEventCallback("action", [this](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &args) {
            if (!args.empty()) on_action(args[0].Get<Rml::String>());
        });
        model = c.GetModelHandle();
        return true;
    }
};

bool App::load_fonts(const std::string &assets_dir)
{
    const char *faces[] = {"Inter-Regular.ttf", "Inter-Medium.ttf", "Inter-SemiBold.ttf", "JetBrainsMono-Medium.ttf"};
    bool ok = true;
    for (const char *f : faces) ok &= Rml::LoadFontFace(assets_dir + "/fonts/" + f, f[0] == 'I' && f[6] == 'R');
    return ok;
}

App::App(Mode mode, Rml::Context *ctx, const std::string &assets_dir, PlatformHooks hooks)
    : d(new Impl(mode, ctx, std::move(hooks)))
{
    d->status = read_status(d->dir);
    d->retire_old_keys();   // v2.6
    read_latest_capture(d->steam_logs, d->capture);
    d->rebuild();
    if (!d->build_model()) return;
    d->doc = ctx->LoadDocument(assets_dir + (mode == Mode::Overlay ? "/overlay.rml" : "/main.rml"));
    if (d->doc) d->doc->Show();
}

App::~App() = default;

bool App::ok() const { return d->doc != nullptr; }

void App::update_context()
{
    d->in_update = true;
    d->ctx->Update();
    d->in_update = false;
}

bool App::tick()
{
    double now = now_seconds();
    if (!d->settings.flush(now)) d->rebuild();   // shows the write error
    if (now >= d->next_poll) {
        d->next_poll = now + 0.25;
        if (d->settings.poll()) d->rebuild();
    }
    if (now >= d->next_status) {
        d->next_status = now + 1.0;
        std::string before = d->capture.file;
        read_latest_capture(d->steam_logs, d->capture);
        if (d->capture.file != before) d->rebuild();
        Status s = read_status(d->dir);
        bool changed = s.live != d->status.live || std::fabs(s.mbps - d->status.mbps) > 0.5 ||
                       std::fabs(s.encode_ms - d->status.encode_ms) > 0.05 || s.width != d->status.width ||
                       std::fabs(s.fps - d->status.fps) > 0.5 || s.gaze_calibrated != d->status.gaze_calibrated ||
                       std::fabs(s.gaze_resid_px - d->status.gaze_resid_px) > 0.05;
        d->status = s;
        if (changed) d->rebuild();
    }
    bool v = d->visible_change;
    d->visible_change = false;
    return v;
}

void App::show(const std::string &id)
{
    if (d->mode == Mode::Overlay) d->tab = id;
    else d->section = id;
    d->rebuild();
}

} // namespace slp
