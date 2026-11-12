// Steam Link Pyrowave - peripheral prefilter + inset locator (PC, D3D11).
// SPDX-License-Identifier: MIT
//
// Per frame, on vrlink's encode thread (its immediate context; nothing here blocks):
//   atlas -> mip 0 of our mipmapped copy -> Gaussian pyramid (v46, slp_pyr.hlsl; was GenerateMips' box chain)
//   mip 4 -> small staging texture (read back two frames later, never waiting) -> locator thread
//   compute shader: per-pixel blur level from eccentricity (gaze = inset centre), contrast restoration,
//   heavy blur of the wide-view area hidden under the inset -> our output texture -> CopyResource(dst)
// Locator thread: finds where the inset (even tiles) sits inside the wide view (odd tiles) and at what scale,
// by matching a downscaled inset against the wide view at mip 4 (zero-mean SAD, sub-pixel refinement).
//
// Settings: the "foveation" section of settings.json (slp_settings.h), re-read live. See kHelp below.

#include "slp_periph.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

void slp_log(const char *fmt, ...);
#include "slp_gaze.h"
#include "slp_status.h"
#include "slp_settings.h"

// v39: latest inset boxes for the encoder (see slp_periph_inset_boxes)
static std::mutex g_box_mu;
static float g_box[2][4] = {};

namespace {

const char kShaderSrc[] =
#include "slp_periph_hlsl.inc"
    ;
// v46: Gaussian pyramid pass (slp_pyr.hlsl)
const char kPyrSrc[] =
#include "slp_pyr_hlsl.inc"
    ;

// ---- settings ----
struct Settings
{
    int enable = 1;            // prefilter on
    float fov = 110.0f;        // wide view field of view, degrees (Steam Frame: ~110)
    float safe = 6.0f;         // full detail within this many degrees of the gaze
    float ramp = 3.0f;         // blur eases in over this many degrees
    float cpd0 = 30.0f;        // acuity at the fovea, cycles/degree
    float e2 = 2.5f;           // eccentricity where acuity has halved, degrees
    float strength = 1.0f;     // 1 = blur matched to acuity; 1.5-2 = more aggressive
    float max_lod = 3.0f;      // strongest blur (mip level)
    float contrast = 0.5f;     // contrast restoration (0 = plain blur)
    int fovea_blur = 1;        // also blur the outer part of the inset
    int hide = 1;              // blur the wide-view area under the inset hard
    float hide_margin = 0.35f; // keep this fraction of the inset half-size at its border untouched
    float hide_lod = 4.0f;
    float safe_unlocated = 30.0f; // wide-view safe radius around the view centre when the gaze is not known
    float inset_scale = 0.0f;  // > 0: fixed inset scale instead of the measured one
    int locator = 0;           // v29: off by default (unstable on real content); gaze assumed at the view centre
    float wide_min_lod = 0.0f; // v29: minimum blur on the whole wide view (anti-aliasing), 0 = none
    int csf = 1;               // per-tile contrast sensitivity in the encoder
    float display_cpd = 11.0f; // display Nyquist, cycles/degree (2160 px / 110 deg ~ 9.8; more at the lens centre)
    int gaze = 1;              // v30: centre the wide-view blur on the gaze SteamVR reports (vrlink's eye tracking)
    float gaze_delay_ms = -1;  // gaze age to use, ms; -1 = calibrated (else 8)
    float gaze_max_age_ms = 250; // older gaze = treat as unknown
    int gaze_now = 1;
    float hide_window_ms = 12; // v35: hide only where the inset is at every gaze time within +-this of the calibrated delay
    float lens_deg = 0;        // v33: blur to flat beyond this many degrees from each eye's straight-ahead (0 = off)
    float lens_feather_deg = 4;
    float chroma_lod = 0;      // v33: extra blur on colour (not brightness) where the periphery is blurred
    int chroma_luma = 1;       // v54: the colour blur keeps linear luminance (luma adjustment)
    int contrast_clamp = 1;    // v33: contrast restoration never overshoots the original pixel          // v31: also keep full detail around the newest gaze (helps after saccades)
    int log = 1;
    // v45: shared "ramp to the edge" (also sent to the Frame and used by the bit weights), and true angles
    float edge_ramp = 0;       // 0 = off, 1 = 3x at edge_end
    float edge_start = 15;     // deg from the gaze where the ramp starts (about the visible inset edge)
    float edge_end = 55;       // deg where it reaches full strength
    float contrast_overshoot = 0; // with contrast_clamp: let restoration go this fraction past the original
    int true_angles = 1;       // wide views: angles and pixel sizes from the eye's (tangent-space) projection
    float temporal = 0;        // v47: periphery stability (0 = off, 1 = 85% of last frame where the clamp allows)
    float temporal_clamp = 1;  // v47: history clamp width in local standard deviations
};

const char kHelp[] =
    "# Steam Link Pyrowave peripheral prefilter (re-read within ~1 s of saving)\n"
    "enable=1\nfov=110\nsafe=6\nramp=3\nstrength=1.0\nmax_lod=3\ncontrast=0.5\nfovea_blur=1\n"
    "hide=1\nhide_margin=0.35\nhide_lod=4\ncsf=1\ndisplay_cpd=11\n";

std::mutex g_set_mu;
Settings g_set;
bool g_set_loaded = false;

std::mutex g_load_mu;

void load_settings()
{
    std::lock_guard<std::mutex> guard(g_load_mu);   // called from vrlink's thread and the encode worker
    // v36: settings come from %LOCALAPPDATA%\SteamLinkPyrowave\settings.json, section "foveation" (slp_settings).
    static uint32_t last_gen = 0;
    uint32_t gen = slp_settings_generation();
    if (g_set_loaded && gen == last_gen) return;
    last_gen = gen;
    const bool has = true;
    Settings s;
    for (auto &kv : slp_settings_section("foveation")) {
        const std::string &k = kv.first;
        const float v = (float)kv.second;
        if (k == "enable") s.enable = v != 0;
        else if (k == "fov") s.fov = std::min(180.0f, std::max(30.0f, v));
        else if (k == "safe") s.safe = std::max(0.0f, v);
        else if (k == "ramp") s.ramp = std::max(0.1f, v);
        else if (k == "cpd0") s.cpd0 = std::max(1.0f, v);
        else if (k == "e2") s.e2 = std::max(0.1f, v);
        else if (k == "strength") s.strength = std::min(4.0f, std::max(0.0f, v));
        else if (k == "max_lod") s.max_lod = std::min(6.0f, std::max(0.0f, v));
        else if (k == "contrast") s.contrast = std::min(2.0f, std::max(0.0f, v));
        else if (k == "fovea_blur") s.fovea_blur = v != 0;
        else if (k == "hide") s.hide = v != 0;
        else if (k == "hide_margin") s.hide_margin = std::min(0.9f, std::max(0.0f, v));
        else if (k == "hide_lod") s.hide_lod = std::min(6.0f, std::max(0.0f, v));
        else if (k == "safe_unlocated") s.safe_unlocated = std::max(0.0f, v);
        else if (k == "inset_scale") s.inset_scale = std::min(1.0f, std::max(0.0f, v));
        else if (k == "locator") s.locator = v != 0;
        else if (k == "wide_min_lod") s.wide_min_lod = std::min(3.0f, std::max(0.0f, v));
        else if (k == "csf") s.csf = v != 0;
        else if (k == "display_cpd") s.display_cpd = std::max(0.0f, v);
        else if (k == "log") s.log = v != 0;
        else if (k == "gaze") s.gaze = v != 0;
        else if (k == "gaze_delay_ms") s.gaze_delay_ms = std::min(200.0f, v);
        else if (k == "gaze_now") s.gaze_now = v != 0;
        else if (k == "hide_window_ms") s.hide_window_ms = std::min(50.0f, std::max(0.0f, v));
        else if (k == "lens_deg") s.lens_deg = std::min(90.0f, std::max(0.0f, v));
        else if (k == "lens_feather_deg") s.lens_feather_deg = std::min(30.0f, std::max(0.0f, v));
        else if (k == "chroma_lod") s.chroma_lod = std::min(3.0f, std::max(0.0f, v));
        else if (k == "contrast_clamp") s.contrast_clamp = v != 0;
        else if (k == "chroma_luma") s.chroma_luma = v != 0;
        else if (k == "edge_ramp") s.edge_ramp = std::min(1.0f, std::max(0.0f, v));
        else if (k == "edge_start") s.edge_start = std::min(80.0f, std::max(0.0f, v));
        else if (k == "edge_end") s.edge_end = std::min(90.0f, std::max(1.0f, v));
        else if (k == "contrast_overshoot") s.contrast_overshoot = std::min(1.0f, std::max(0.0f, v));
        else if (k == "true_angles") s.true_angles = v != 0;
        else if (k == "temporal") s.temporal = std::min(1.0f, std::max(0.0f, v));
        else if (k == "temporal_clamp") s.temporal_clamp = std::min(4.0f, std::max(0.0f, v));
        else if (k == "gaze_max_age_ms") s.gaze_max_age_ms = std::max(10.0f, v);
    }
    {
        std::lock_guard<std::mutex> lk(g_set_mu);
        g_set = s;
        g_set_loaded = true;
    }
    slp_log("[PERIPH] settings%s: enable %d, fov %.0f, safe %.1f deg (+%.1f ramp), strength %.2f, max lod %.1f, contrast %.2f, "
            "inset blur %d, hide %d (margin %.2f, lod %.1f), csf %d (display %.1f cpd), locator %d (wide safe %.0f deg "
            "around the view centre when not located), wide min lod %.2f, inset scale %.2f",
            has ? " (settings.json)" : " (defaults)", s.enable, s.fov, s.safe, s.ramp, s.strength, s.max_lod, s.contrast,
            s.fovea_blur, s.hide, s.hide_margin, s.hide_lod, s.csf, s.display_cpd, s.locator, s.safe_unlocated,
            s.wide_min_lod, s.inset_scale);
    slp_log("[PERIPH] v33: lens_deg %.1f (feather %.1f), chroma_lod %.2f (keeps luminance %d), contrast_clamp %d", s.lens_deg,
            s.lens_feather_deg, s.chroma_lod, s.chroma_luma, s.contrast_clamp);
    slp_log("[PERIPH] v45: edge ramp %.2f (%.0f-%.0f deg), contrast overshoot %.2f, true angles %d", s.edge_ramp,
            s.edge_start, s.edge_end, s.contrast_overshoot, s.true_angles);
    slp_log("[PERIPH] v47: periphery stability %.2f (clamp %.2f)", s.temporal, s.temporal_clamp);
    slp_log("[PERIPH] gaze %d, gaze_now %d (delay %s%.0f ms, max age %.0f ms)", s.gaze, s.gaze_now,
            s.gaze_delay_ms < 0 ? "calibrated, default " : "", s.gaze_delay_ms < 0 ? 8.0f : s.gaze_delay_ms, s.gaze_max_age_ms);
}

Settings settings()
{
    std::lock_guard<std::mutex> lk(g_set_mu);
    return g_set;
}

// ---- locator ----
constexpr int kLocMip = 4;

struct Estimate
{
    bool valid = false;
    float scale = 0.0f;          // inset size / wide view size
    float cx[2] = {}, cy[2] = {}; // inset centre in the wide view, full-res px
    float quality = 0.0f;
};

std::mutex g_loc_mu;
std::condition_variable g_loc_cv;
std::vector<float> g_loc_in;     // luma of the whole atlas at mip kLocMip (n x 4n)
int g_loc_n = 0;
bool g_loc_has = false, g_loc_quit = false;
double g_loc_t = 0;              // slp_gaze_now() when that frame was prefiltered
Estimate g_est;
std::thread g_loc_thread;

// ---- gaze -> inset centre (v30) ----
// The inset is rendered around the gaze, and the wide view is a linear (tangent-space) projection, so per eye
// cx = ax * tan_x + bx and cy = ay * tan_y + by. Defaults come from the FOV (symmetric projection, y down); when the
// locator runs, its confident matches calibrate the fit and the gaze delay.
const float kDelays[] = {0, 4, 8, 12, 16, 24, 32, 48};
constexpr int kNDelay = sizeof(kDelays) / sizeof(kDelays[0]);
constexpr int kCalN = 400;

struct GazeFit
{
    bool valid = false;
    float ax[2] = {}, bx[2] = {}, ay[2] = {}, by[2] = {};
    float delay_ms = 8.0f;
    float resid_px = 0;
    int n = 0;
    float T = 960;   // tile size the fit was made at (v32: rescaled when the stream width changes)
};
std::mutex g_fit_mu;
GazeFit g_fit;

struct CalSample { float tx, ty, cx[2], cy[2]; };
std::vector<CalSample> g_cal[kNDelay];   // locator thread only
uint64_t g_cal_pos = 0;

// v34: what a Steam Frame fit looks like (two good calibrations at 960 and 512, as fractions of the tile size).
// Used before a fit exists, and to reject nonsense fits (v33 saved one with the lens centre near the tile edge).
GazeFit frame_default(float T)
{
    GazeFit f;
    f.valid = true;
    f.T = T;
    f.ax[0] = 0.32f * T; f.bx[0] = 0.58f * T; f.ay[0] = -0.33f * T; f.by[0] = 0.40f * T;   // v35: 832 calibration
    f.ax[1] = 0.32f * T; f.bx[1] = 0.42f * T; f.ay[1] = -0.33f * T; f.by[1] = 0.40f * T;
    f.delay_ms = 16;
    return f;
}

bool plausible(const GazeFit &f, const char **why)
{
    float T = f.T;
    for (int e = 0; e < 2; ++e) {
        if (!(f.ax[e] > 0.12f * T && f.ax[e] < 0.45f * T)) { *why = "x slope"; return false; }
        if (!(f.ay[e] < -0.15f * T && f.ay[e] > -0.50f * T)) { *why = "y slope"; return false; }
        if (!(f.bx[e] > 0.25f * T && f.bx[e] < 0.75f * T)) { *why = "x centre"; return false; }
        if (!(f.by[e] > 0.20f * T && f.by[e] < 0.65f * T)) { *why = "y centre"; return false; }
    }
    if (std::fabs(f.by[0] - f.by[1]) > 0.08f * T) { *why = "eyes at different heights"; return false; }
    return true;
}

// v31: the fit is saved to gaze_fit.txt in the data folder and loaded at start, so the locator can stay off afterwards.
bool fit_path(char *path)
{
    std::string p = slp_data_path("gaze_fit.txt");   // v36: was %TEMP%\slp_gaze_fit.txt
    if (p.size() >= MAX_PATH) return false;
    strcpy(path, p.c_str());
    return true;
}

void save_fit(const GazeFit &f)
{
    char path[MAX_PATH + 32];
    if (!fit_path(path)) return;
    if (FILE *o = fopen(path, "w")) {
        fprintf(o, "%g %g %g %g %g %g %g %g %g %g %d %g\n", f.ax[0], f.bx[0], f.ay[0], f.by[0], f.ax[1], f.bx[1], f.ay[1],
                f.by[1], f.delay_ms, f.resid_px, f.n, f.T);
        fclose(o);
    }
}

GazeFit gaze_fit()
{
    std::lock_guard<std::mutex> lk(g_fit_mu);
    static bool tried = false;
    if (!g_fit.valid && !tried) {
        tried = true;
        char path[MAX_PATH + 32];
        if (fit_path(path))
            if (FILE *f = fopen(path, "r")) {
                GazeFit l;
                if (fscanf(f, "%f %f %f %f %f %f %f %f %f %f %d %f", &l.ax[0], &l.bx[0], &l.ay[0], &l.by[0], &l.ax[1], &l.bx[1],
                           &l.ay[1], &l.by[1], &l.delay_ms, &l.resid_px, &l.n, &l.T) >= 11) {
                    if (!(l.T > 16)) l.T = 960;
                    const char *why = "";
                    if (plausible(l, &why)) {
                        l.valid = true;
                        g_fit = l;
                        slp_log("[GAZE] loaded the saved fit (delay %.0f ms, resid %.1f px)", l.delay_ms, l.resid_px);
                    } else {
                        slp_log("[GAZE] ignored the saved fit (implausible %s); using the Steam Frame default until calibrated", why);
                    }
                }
                fclose(f);
            }
    }
    return g_fit;
}

// least squares y = a x + b; returns rms residual, false if x has too little spread
bool fit_line(const std::vector<float> &x, const std::vector<float> &y, float *a, float *b, float *rms)
{
    size_t n = x.size();
    double mx = 0, my = 0;
    for (size_t i = 0; i < n; ++i) { mx += x[i]; my += y[i]; }
    mx /= n; my /= n;
    double sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; ++i) { sxx += (x[i] - mx) * (x[i] - mx); sxy += (x[i] - mx) * (y[i] - my); }
    if (sxx / n < 0.10 * 0.10) return false;   // v34: the gaze must have covered about +-6 deg or more
    *a = (float)(sxy / sxx);
    *b = (float)(my - *a * mx);
    // v34: second pass without outliers (locator mismatches)
    std::vector<float> ad(n);
    for (size_t i = 0; i < n; ++i) ad[i] = std::fabs(y[i] - (*a * x[i] + *b));
    std::vector<float> sorted = ad;
    std::nth_element(sorted.begin(), sorted.begin() + n / 2, sorted.end());
    float lim = std::max(2.5f * sorted[n / 2], 4.0f);
    double sx = 0, sy = 0, cnt = 0;
    for (size_t i = 0; i < n; ++i) if (ad[i] <= lim) { sx += x[i]; sy += y[i]; cnt++; }
    if (cnt < 0.6 * n) return false;
    mx = sx / cnt; my = sy / cnt;
    sxx = 0; sxy = 0;
    for (size_t i = 0; i < n; ++i) if (ad[i] <= lim) { sxx += (x[i] - mx) * (x[i] - mx); sxy += (x[i] - mx) * (y[i] - my); }
    if (sxx / cnt < 0.10 * 0.10) return false;
    *a = (float)(sxy / sxx);
    *b = (float)(my - *a * mx);
    double r = 0;
    for (size_t i = 0; i < n; ++i) if (ad[i] <= lim) { double d = y[i] - (*a * x[i] + *b); r += d * d; }
    *rms = (float)std::sqrt(r / cnt);
    return true;
}

void calibrate_add(double frame_t, const float cx[2], const float cy[2])
{
    for (int d = 0; d < kNDelay; ++d) {
        SlpGaze g;
        if (!slp_gaze_at(frame_t - kDelays[d] * 1e-3, &g) || frame_t - kDelays[d] * 1e-3 - g.t > 0.05) continue;
        CalSample c{g.x / -g.z, g.y / -g.z, {cx[0], cx[1]}, {cy[0], cy[1]}};
        auto &v = g_cal[d];
        if (v.size() < (size_t)kCalN) v.push_back(c);
        else v[g_cal_pos % kCalN] = c;
    }
    g_cal_pos++;
}

void calibrate_fit(int T)
{
    static int last_T = 0;
    if (T != last_T) {   // v34: samples are in pixels of one stream width
        for (auto &v : g_cal) v.clear();
        last_T = T;
    }
    GazeFit best;
    float best_r = 1e30f;
    std::vector<float> tx, ty, c;
    for (int d = 0; d < kNDelay; ++d) {
        auto &v = g_cal[d];
        if (v.size() < 150) continue;
        GazeFit f;
        f.delay_ms = kDelays[d];
        f.n = (int)v.size();
        float r2 = 0;
        bool ok = true;
        tx.clear(); ty.clear();
        for (auto &s : v) { tx.push_back(s.tx); ty.push_back(s.ty); }
        for (int e = 0; e < 2 && ok; ++e) {
            float r;
            c.clear(); for (auto &s : v) c.push_back(s.cx[e]);
            ok = fit_line(tx, c, &f.ax[e], &f.bx[e], &r); r2 += r * r;
            c.clear(); for (auto &s : v) c.push_back(s.cy[e]);
            ok = ok && fit_line(ty, c, &f.ay[e], &f.by[e], &r); r2 += r * r;
        }
        if (!ok) continue;
        f.resid_px = std::sqrt(r2 / 4);
        if (f.resid_px < best_r) { best_r = f.resid_px; best = f; }
    }
    // trust it when the residual is small next to the inset (locator noise is ~1 mip-4 px = 16 px)
    best.T = (float)T;
    const char *why = "";
    best.valid = best.n > 0 && best.resid_px < 0.04f * T && plausible(best, &why);
    static int rejected = 0;
    if (best.n > 0 && !best.valid && rejected++ % 15 == 0)
        slp_log("[GAZE] calibration not accepted yet: n %d, resid %.1f px (limit %.1f)%s%s", best.n, best.resid_px, 0.04f * T,
                *why ? ", implausible " : "", why);
    // v38: keep a good fit. A new one replaces it only when about as good (eye tracking dropping out or a few
    // bad locator matches can otherwise swap a 6 px fit for a 10 px one), or after 10 minutes so a headset that
    // moved on the face can still re-calibrate.
    static auto kept_at = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (best.valid) {
        GazeFit cur;
        { std::lock_guard<std::mutex> lk(g_fit_mu); cur = g_fit; }
        bool aged = now - kept_at > std::chrono::seconds(600);
        if (cur.valid && cur.T == best.T && best.resid_px > cur.resid_px * 1.15f + 0.5f && !aged) {
            static int held = 0;
            if (held++ % 30 == 0)
                slp_log("[GAZE] keeping the current fit (resid %.1f px) over a worse new one (%.1f px)", cur.resid_px, best.resid_px);
            return;
        }
        kept_at = now;
        save_fit(best);
    }
    std::lock_guard<std::mutex> lk(g_fit_mu);
    if (best.valid) g_fit = best;
}

struct Match { float err = 1e30f; float x = 0, y = 0; int m = 0; float median = 0; };

// Best position of the inset (n x n, scaled to m x m) inside the wide view (n x n), zero-mean SAD.
Match match_scale(const float *wide, const float *ins, int n, int m, int cx0, int cy0, int radius, float *tex_std)
{
    Match best;
    if (m < 4 || m > n) return best;
    std::vector<float> t((size_t)m * m);
    double mean = 0;
    for (int j = 0; j < m; ++j)
        for (int i = 0; i < m; ++i) {
            float sx = (i + 0.5f) * n / m - 0.5f, sy = (j + 0.5f) * n / m - 0.5f;
            int x0 = std::max(0, std::min(n - 1, (int)std::floor(sx))), y0 = std::max(0, std::min(n - 1, (int)std::floor(sy)));
            int x1 = std::min(n - 1, x0 + 1), y1 = std::min(n - 1, y0 + 1);
            float fx = std::min(1.0f, std::max(0.0f, sx - x0)), fy = std::min(1.0f, std::max(0.0f, sy - y0));
            float v = (ins[y0 * n + x0] * (1 - fx) + ins[y0 * n + x1] * fx) * (1 - fy) +
                      (ins[y1 * n + x0] * (1 - fx) + ins[y1 * n + x1] * fx) * fy;
            t[(size_t)j * m + i] = v;
            mean += v;
        }
    mean /= (double)m * m;
    double var = 0;
    for (float &v : t) { v -= (float)mean; var += (double)v * v; }
    float sd = (float)std::sqrt(var / ((double)m * m));
    if (tex_std) *tex_std = sd;
    // integral image of the wide view for window means
    std::vector<double> ii((size_t)(n + 1) * (n + 1), 0.0);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            ii[(size_t)(y + 1) * (n + 1) + x + 1] = wide[y * n + x] + ii[(size_t)y * (n + 1) + x + 1] +
                                                    ii[(size_t)(y + 1) * (n + 1) + x] - ii[(size_t)y * (n + 1) + x];
    int lo_x = 0, hi_x = n - m, lo_y = 0, hi_y = n - m;
    if (radius >= 0) {
        lo_x = std::max(0, cx0 - radius); hi_x = std::min(n - m, cx0 + radius);
        lo_y = std::max(0, cy0 - radius); hi_y = std::min(n - m, cy0 + radius);
    }
    if (hi_x < lo_x || hi_y < lo_y) return best;
    int W = hi_x - lo_x + 1, Hh = hi_y - lo_y + 1;
    std::vector<float> err((size_t)W * Hh);
    for (int y = lo_y; y <= hi_y; ++y)
        for (int x = lo_x; x <= hi_x; ++x) {
            double s = ii[(size_t)(y + m) * (n + 1) + x + m] - ii[(size_t)y * (n + 1) + x + m] -
                       ii[(size_t)(y + m) * (n + 1) + x] + ii[(size_t)y * (n + 1) + x];
            float wm = (float)(s / ((double)m * m));
            float sad = 0;
            for (int j = 0; j < m; ++j) {
                const float *wr = wide + (size_t)(y + j) * n + x;
                const float *tr = t.data() + (size_t)j * m;
                for (int i = 0; i < m; ++i) sad += std::fabs(wr[i] - wm - tr[i]);
            }
            err[(size_t)(y - lo_y) * W + (x - lo_x)] = sad / ((float)m * m);
        }
    int bi = 0;
    for (size_t i = 1; i < err.size(); ++i) if (err[i] < err[bi]) bi = (int)i;
    int bx = bi % W, by = bi / W;
    std::vector<float> sorted = err;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    best.median = sorted[sorted.size() / 2];
    best.err = err[bi];
    best.m = m;
    // sub-pixel: parabola through the neighbours
    auto sub = [](float a, float b, float c) { float d = a - 2 * b + c; return d > 1e-9f ? 0.5f * (a - c) / d : 0.0f; };
    float ox = (bx > 0 && bx < W - 1) ? sub(err[(size_t)by * W + bx - 1], err[(size_t)by * W + bx], err[(size_t)by * W + bx + 1]) : 0;
    float oy = (by > 0 && by < Hh - 1) ? sub(err[(size_t)(by - 1) * W + bx], err[(size_t)by * W + bx], err[(size_t)(by + 1) * W + bx]) : 0;
    best.x = lo_x + bx + std::max(-0.5f, std::min(0.5f, ox));
    best.y = lo_y + by + std::max(-0.5f, std::min(0.5f, oy));
    return best;
}

void locator_main()
{
    std::vector<float> img;
    int n = 0;
    double frame_t = 0;
    auto last_fit = std::chrono::steady_clock::now();
    int cur_m = 0;
    float cur_x[2] = {0, 0}, cur_y[2] = {0, 0};
    auto last_full = std::chrono::steady_clock::now() - std::chrono::seconds(100);
    auto last_good = std::chrono::steady_clock::now() - std::chrono::seconds(100);
    auto last_log = std::chrono::steady_clock::now();
    uint64_t frames = 0, good = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g_loc_mu);
            g_loc_cv.wait(lk, [] { return g_loc_has || g_loc_quit; });
            if (g_loc_quit) return;
            img.swap(g_loc_in);
            n = g_loc_n;
            frame_t = g_loc_t;
            g_loc_has = false;
        }
        if (n < 16 || img.size() < (size_t)n * n * 4) continue;
        Settings s = settings();
        frames++;
        // Even tiles are the gaze insets, odd tiles the wide views (measured on a real atlas grab, v26).
        const float *ins[2] = {img.data(), img.data() + (size_t)2 * n * n};
        const float *wide[2] = {img.data() + (size_t)n * n, img.data() + (size_t)3 * n * n};
        auto now = std::chrono::steady_clock::now();
        // v32: search the whole view until located, and again every 2 s (v29-v31 never did a full search with a
        // fixed inset_scale, so the match stayed stuck near the top-left corner).
        bool lost = now - last_good > std::chrono::milliseconds(500);
        bool full = cur_m == 0 || lost || now - last_full > std::chrono::seconds(2);
        int m_lo, m_hi;
        if (s.inset_scale > 0) { m_lo = m_hi = std::max(4, (int)std::lround(s.inset_scale * n)); }
        else if (full) { m_lo = std::max(6, (int)(0.15f * n)); m_hi = (int)(0.70f * n); }
        else { m_lo = std::max(6, cur_m - 1); m_hi = std::min((int)(0.70f * n), cur_m + 1); }
        Match best[2];
        float best_total = 1e30f;
        float tex[2] = {0, 0};
        for (int m = m_lo; m <= m_hi; ++m) {
            Match mm[2];
            float tsd[2];
            for (int e = 0; e < 2; ++e) {
                int radius = full ? -1 : std::max(4, n / 5);
                mm[e] = match_scale(wide[e], ins[e], n, m, (int)std::lround(cur_x[e]), (int)std::lround(cur_y[e]), radius, &tsd[e]);
            }
            // Compare scales by error relative to the template's own contrast (a tiny, washed-out template
            // would otherwise always win).
            float total = mm[0].err / std::max(tsd[0], 1e-3f) + mm[1].err / std::max(tsd[1], 1e-3f);
            if (total < best_total) { best_total = total; best[0] = mm[0]; best[1] = mm[1]; tex[0] = tsd[0]; tex[1] = tsd[1]; }
        }
        if (full) last_full = now;
        // Trust it only when the match is clearly better than a random position and the inset has detail.
        bool ok = best[0].m > 0;
        float q = 0;
        for (int e = 0; e < 2 && ok; ++e) {
            float ratio = best[e].median > 0 ? best[e].err / best[e].median : 1.0f;
            float rel = best[e].err / std::max(tex[e], 1e-3f);
            q = std::max(q, ratio);
            if (ratio > 0.4f || rel > 0.3f || tex[e] < 0.004f) ok = false;
        }
        Estimate est;
        if (ok) {
            cur_m = best[0].m;
            for (int e = 0; e < 2; ++e) { cur_x[e] = best[e].x; cur_y[e] = best[e].y; }
            last_good = now;
            good++;
            float k = (float)(1 << kLocMip);
            est.valid = true;
            est.scale = (float)cur_m / n;
            for (int e = 0; e < 2; ++e) {
                est.cx[e] = (cur_x[e] + cur_m * 0.5f) * k;
                est.cy[e] = (cur_y[e] + cur_m * 0.5f) * k;
            }
            est.quality = 1.0f - q;
            if (s.gaze) calibrate_add(frame_t, est.cx, est.cy);
        } else {
            // Keep the last good estimate for half a second (blinks, flat frames), then give up.
            std::lock_guard<std::mutex> lk(g_loc_mu);
            est = g_est;
            if (now - last_good > std::chrono::milliseconds(500)) { est.valid = false; cur_m = s.inset_scale > 0 ? cur_m : 0; }
        }
        {
            std::lock_guard<std::mutex> lk(g_loc_mu);
            g_est = est;
        }
        if (s.gaze && now - last_fit > std::chrono::seconds(2)) {
            last_fit = now;
            calibrate_fit(n << kLocMip);
        }
        if (s.log && now - last_log > std::chrono::seconds(10)) {
            last_log = now;
            float ppd_w = n * (1 << kLocMip) / s.fov;
            slp_log("[PERIPH] inset %s: scale %.3f (inset %.0f of %.0f deg, %.1f vs %.1f px/deg), eye0 (%.0f, %.0f) eye1 (%.0f, %.0f) "
                    "px, match %.2f; located %.0f%% of %llu frames",
                    est.valid ? "located" : "NOT located", est.scale, est.scale * s.fov, s.fov,
                    est.scale > 0 ? ppd_w / est.scale : 0.0f, ppd_w, est.cx[0], est.cy[0], est.cx[1], est.cy[1], est.quality,
                    frames ? 100.0 * good / frames : 0.0, (unsigned long long)frames);
            frames = good = 0;
        }
    }
}

Estimate estimate()
{
    std::lock_guard<std::mutex> lk(g_loc_mu);
    return g_est;
}

// ---- D3D11 side (vrlink's encode thread only) ----
struct Res
{
    bool failed = false;
    UINT w = 0, h = 0;
    ID3D11Device *dev = nullptr;
    ID3D11Texture2D *mip = nullptr;
    ID3D11ShaderResourceView *mip_srv = nullptr;
    // v46: Gaussian pyramid: one-level views (SRV of level k - 1, UAV of level k) and its compute shader
    ID3D11ShaderResourceView *lvl_srv[kLocMip + 3] = {};
    ID3D11UnorderedAccessView *lvl_uav[kLocMip + 3] = {};
    ID3D11ComputeShader *pyr_cs = nullptr;
    // v47: last frame's output for the periphery stability blend
    ID3D11Texture2D *hist = nullptr;
    ID3D11ShaderResourceView *hist_srv = nullptr;
    uint64_t hist_frame = 0;   // g_r.frame when hist was written (0 = never)
    ID3D11Texture2D *out = nullptr;
    ID3D11UnorderedAccessView *out_uav = nullptr;
    ID3D11ComputeShader *cs = nullptr;
    ID3D11Buffer *cb = nullptr;
    ID3D11SamplerState *samp = nullptr;
    ID3D11Texture2D *stage[3] = {};
    bool stage_full[3] = {};
    double stage_t[3] = {};
    uint64_t frame = 0;
} g_r;

template <typename T> void rel(T *&p) { if (p) { p->lpVtbl->Release(p); p = nullptr; } }

void release_sized()
{
    rel(g_r.mip_srv); rel(g_r.mip); rel(g_r.out_uav); rel(g_r.out);
    for (auto &v : g_r.lvl_srv) rel(v);
    for (auto &v : g_r.lvl_uav) rel(v);
    rel(g_r.hist_srv); rel(g_r.hist); g_r.hist_frame = 0;
    for (auto &s : g_r.stage) rel(s);
    for (bool &b : g_r.stage_full) b = false;
    g_r.w = g_r.h = 0;
}

bool compile_shader(ID3D11Device *dev)
{
    HMODULE h = LoadLibraryA("d3dcompiler_47.dll");
    if (!h) { slp_log("[PERIPH] d3dcompiler_47.dll not found"); return false; }
    auto compile = reinterpret_cast<pD3DCompile>(reinterpret_cast<void *>(GetProcAddress(h, "D3DCompile")));
    if (!compile) { slp_log("[PERIPH] D3DCompile not found"); return false; }
    ID3DBlob *code = nullptr, *errs = nullptr;
    HRESULT hr = compile(kShaderSrc, sizeof(kShaderSrc) - 1, "slp_periph.hlsl", nullptr, nullptr, "main", "cs_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errs);
    if (FAILED(hr) || !code) {
        slp_log("[PERIPH] shader compile failed 0x%08lx: %s", (unsigned long)hr,
                errs ? (const char *)errs->lpVtbl->GetBufferPointer(errs) : "(no message)");
        rel(errs); rel(code);
        return false;
    }
    rel(errs);
    hr = dev->lpVtbl->CreateComputeShader(dev, code->lpVtbl->GetBufferPointer(code), code->lpVtbl->GetBufferSize(code),
                                          nullptr, &g_r.cs);
    rel(code);
    if (FAILED(hr)) { slp_log("[PERIPH] CreateComputeShader failed 0x%08lx", (unsigned long)hr); return false; }
    // v46: the Gaussian pyramid pass
    code = nullptr; errs = nullptr;
    hr = compile(kPyrSrc, sizeof(kPyrSrc) - 1, "slp_pyr.hlsl", nullptr, nullptr, "main", "cs_5_0",
                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errs);
    if (FAILED(hr) || !code) {
        slp_log("[PERIPH] pyramid shader compile failed 0x%08lx: %s", (unsigned long)hr,
                errs ? (const char *)errs->lpVtbl->GetBufferPointer(errs) : "(no message)");
        rel(errs); rel(code);
        return false;
    }
    rel(errs);
    hr = dev->lpVtbl->CreateComputeShader(dev, code->lpVtbl->GetBufferPointer(code), code->lpVtbl->GetBufferSize(code),
                                          nullptr, &g_r.pyr_cs);
    rel(code);
    if (FAILED(hr)) { slp_log("[PERIPH] CreateComputeShader (pyramid) failed 0x%08lx", (unsigned long)hr); return false; }
    return true;
}

bool ensure(ID3D11DeviceContext *ctx, const D3D11_TEXTURE2D_DESC &ad)
{
    if (g_r.failed) return false;
    if (!g_r.dev) {
        ctx->lpVtbl->GetDevice(ctx, &g_r.dev);
        if (!g_r.dev) { g_r.failed = true; return false; }
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 15 * 16;   // v45: 15 float4 (see slp_periph.hlsl)
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (!compile_shader(g_r.dev) || FAILED(g_r.dev->lpVtbl->CreateBuffer(g_r.dev, &bd, nullptr, &g_r.cb)) ||
            FAILED(g_r.dev->lpVtbl->CreateSamplerState(g_r.dev, &sd, &g_r.samp))) {
            slp_log("[PERIPH] setup failed; prefilter off for this session");
            g_r.failed = true;
            return false;
        }
        g_loc_thread = std::thread(locator_main);
        g_loc_thread.detach();
    }
    if (g_r.w == ad.Width && g_r.h == ad.Height) return true;
    release_sized();
    ID3D11Device *dev = g_r.dev;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = ad.Width; d.Height = ad.Height; d.MipLevels = kLocMip + 3; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;   // v46: our own pyramid, no GenerateMips
    d.MiscFlags = 0;
    bool ok = SUCCEEDED(dev->lpVtbl->CreateTexture2D(dev, &d, nullptr, &g_r.mip)) &&
              SUCCEEDED(dev->lpVtbl->CreateShaderResourceView(dev, (ID3D11Resource *)g_r.mip, nullptr, &g_r.mip_srv));
    for (UINT k = 0; ok && k < (UINT)(kLocMip + 3); ++k) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = d.Format; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MostDetailedMip = k; sv.Texture2D.MipLevels = 1;
        D3D11_UNORDERED_ACCESS_VIEW_DESC uv = {};
        uv.Format = d.Format; uv.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D; uv.Texture2D.MipSlice = k;
        ok = SUCCEEDED(dev->lpVtbl->CreateShaderResourceView(dev, (ID3D11Resource *)g_r.mip, &sv, &g_r.lvl_srv[k])) &&
             SUCCEEDED(dev->lpVtbl->CreateUnorderedAccessView(dev, (ID3D11Resource *)g_r.mip, &uv, &g_r.lvl_uav[k]));
    }
    D3D11_TEXTURE2D_DESC o = d;
    o.MipLevels = 1; o.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE; o.MiscFlags = 0;
    ok = ok && SUCCEEDED(dev->lpVtbl->CreateTexture2D(dev, &o, nullptr, &g_r.hist)) &&
         SUCCEEDED(dev->lpVtbl->CreateShaderResourceView(dev, (ID3D11Resource *)g_r.hist, nullptr, &g_r.hist_srv));
    ok = ok && SUCCEEDED(dev->lpVtbl->CreateTexture2D(dev, &o, nullptr, &g_r.out)) &&
         SUCCEEDED(dev->lpVtbl->CreateUnorderedAccessView(dev, (ID3D11Resource *)g_r.out, nullptr, &g_r.out_uav));
    D3D11_TEXTURE2D_DESC st = d;
    st.Width = ad.Width >> kLocMip; st.Height = ad.Height >> kLocMip; st.MipLevels = 1; st.BindFlags = 0; st.MiscFlags = 0;
    st.Usage = D3D11_USAGE_STAGING; st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto &s : g_r.stage) ok = ok && SUCCEEDED(dev->lpVtbl->CreateTexture2D(dev, &st, nullptr, &s));
    if (!ok) {
        slp_log("[PERIPH] could not create %ux%u resources; prefilter off", ad.Width, ad.Height);
        release_sized();
        g_r.failed = true;
        return false;
    }
    g_r.w = ad.Width; g_r.h = ad.Height;
    slp_log("[PERIPH] prefilter ready for %ux%u (tile %u px, locator at %ux%u)", ad.Width, ad.Height, ad.Width,
            ad.Width >> kLocMip, ad.Height >> kLocMip);
    return true;
}

float half_to_float(uint16_t h)
{
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    float v;
    if (e == 0) v = std::ldexp((float)m, -24);
    else if (e == 31) v = 0.0f;
    else v = std::ldexp((float)(m | 0x400), (int)e - 25);
    return s ? -v : v;
}

void readback_for_locator(ID3D11DeviceContext *ctx)
{
    int cur = (int)(g_r.frame % 3), old = (int)((g_r.frame + 1) % 3);   // written two frames ago
    ctx->lpVtbl->CopySubresourceRegion(ctx, (ID3D11Resource *)g_r.stage[cur], 0, 0, 0, 0, (ID3D11Resource *)g_r.mip,
                                       kLocMip, nullptr);
    g_r.stage_full[cur] = true;
    g_r.stage_t[cur] = slp_gaze_now();
    if (!g_r.stage_full[old]) return;
    D3D11_MAPPED_SUBRESOURCE ms;
    if (ctx->lpVtbl->Map(ctx, (ID3D11Resource *)g_r.stage[old], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms) != S_OK)
        return;   // not ready: skip, never stall vrlink
    int n = (int)(g_r.w >> kLocMip), rows = (int)(g_r.h >> kLocMip);
    std::vector<float> luma((size_t)n * rows);
    for (int y = 0; y < rows; ++y) {
        const uint16_t *r = (const uint16_t *)((const uint8_t *)ms.pData + (size_t)y * ms.RowPitch);
        for (int x = 0; x < n; ++x)
            luma[(size_t)y * n + x] = 0.2126f * half_to_float(r[4 * x]) + 0.7152f * half_to_float(r[4 * x + 1]) +
                                      0.0722f * half_to_float(r[4 * x + 2]);
    }
    ctx->lpVtbl->Unmap(ctx, (ID3D11Resource *)g_r.stage[old], 0);
    {
        std::lock_guard<std::mutex> lk(g_loc_mu);
        g_loc_in.swap(luma);
        g_loc_n = n;
        g_loc_t = g_r.stage_t[old];
        g_loc_has = true;
    }
    g_loc_cv.notify_one();
}

} // namespace

static SlpRampInfo g_ramp;   // guarded by g_box_mu

bool slp_periph_process(ID3D11DeviceContext *ctx, ID3D11Texture2D *atlas, ID3D11Texture2D *dst)
{
    load_settings();
    Settings s = settings();
    if (!s.enable || !ctx || !atlas || !dst) return false;
    D3D11_TEXTURE2D_DESC ad;
    atlas->lpVtbl->GetDesc(atlas, &ad);
    if (ad.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || ad.Height != 4 * ad.Width || (ad.Width % (1u << kLocMip)) != 0)
        return false;
    if (!ensure(ctx, ad)) return false;

    ctx->lpVtbl->CopySubresourceRegion(ctx, (ID3D11Resource *)g_r.mip, 0, 0, 0, 0, (ID3D11Resource *)atlas, 0, nullptr);
    g_r.frame++;   // v46: the pyramid is built below with the main pass; the locator readback follows it

    Estimate est = s.locator ? estimate() : Estimate{};
    float T = (float)ad.Width;
    float scale = s.inset_scale > 0 ? s.inset_scale : (est.valid ? est.scale : 0.29f);
    // Centre: the gaze from SteamVR (calibrated fit, else the FOV model), else the locator, else unknown.
    float cx[2] = {est.cx[0], est.cx[1]}, cy[2] = {est.cy[0], est.cy[1]};
    bool valid = est.valid, confident = est.valid;
    const char *src = est.valid ? "locator" : "none";
    SlpGaze g;
    GazeFit fit = gaze_fit();
    if (fit.valid && fit.T != T) {   // fit made at another stream width: scale to this one
        float k = T / fit.T;
        for (int e = 0; e < 2; ++e) { fit.ax[e] *= k; fit.bx[e] *= k; fit.ay[e] *= k; fit.by[e] *= k; }
        fit.resid_px *= k;
        fit.T = T;
    }
    // v34: until calibrated, use the Steam Frame default mapping (much closer than the symmetric FOV model)
    bool calibrated = fit.valid;
    if (g_r.frame % 64 == 1) {
        slp_status_set("width", T);
        slp_status_set("gaze_calibrated", calibrated ? 1 : 0);
        slp_status_set("gaze_resid_px", calibrated ? fit.resid_px : -1);
        slp_status_set("gaze_delay_ms", calibrated ? fit.delay_ms : -1);
        slp_status_set("backend", 54);
    }
    if (!fit.valid) fit = frame_default(T);
    double now = slp_gaze_now();
    float delay = s.gaze_delay_ms >= 0 ? s.gaze_delay_ms : (fit.valid ? fit.delay_ms : 8.0f);
    // v31: also where the eye is now (newest sample), for the union of "inset" and "now" sharp regions
    float nx[2] = {0, 0}, ny[2] = {0, 0};
    bool now_ok = false;
    SlpGaze gn;
    if (s.gaze && s.gaze_now && fit.valid && slp_gaze_at(now, &gn) && now - gn.t < s.gaze_max_age_ms * 1e-3) {
        float tx = gn.x / -gn.z, ty = gn.y / -gn.z;
        for (int e = 0; e < 2; ++e) {
            nx[e] = std::min((float)ad.Width, std::max(0.0f, fit.ax[e] * tx + fit.bx[e]));
            ny[e] = std::min((float)ad.Width, std::max(0.0f, fit.ay[e] * ty + fit.by[e]));
        }
        now_ok = true;
    }
    if (s.gaze && slp_gaze_at(now - delay * 1e-3, &g) && now - g.t < s.gaze_max_age_ms * 1e-3) {
        float tx = g.x / -g.z, ty = g.y / -g.z;
        if (fit.valid) {
            for (int e = 0; e < 2; ++e) { cx[e] = fit.ax[e] * tx + fit.bx[e]; cy[e] = fit.ay[e] * ty + fit.by[e]; }
            confident = true;
            src = calibrated ? "gaze (calibrated)" : "gaze (Steam Frame default)";
        } else {
            float th = std::tan(s.fov * 0.5f * 3.14159265f / 180.0f);
            for (int e = 0; e < 2; ++e) { cx[e] = 0.5f * T * (1 + tx / th); cy[e] = 0.5f * T * (1 - ty / th); }
            confident = false;   // FOV model: good enough for the acuity blur, not for hiding
            src = "gaze (FOV model)";
        }
        for (int e = 0; e < 2; ++e) { cx[e] = std::min(T, std::max(0.0f, cx[e])); cy[e] = std::min(T, std::max(0.0f, cy[e])); }
        valid = true;
    }
    {
        static auto last = std::chrono::steady_clock::now();
        auto tn = std::chrono::steady_clock::now();
        if (s.log && tn - last > std::chrono::seconds(10)) {
            last = tn;
            uint64_t calls, vcalls; bool hooked;
            slp_gaze_stats(&calls, &vcalls, &hooked);
            slp_log("[GAZE] hook %d, %llu updates (%llu valid); centre from %s: eye0 (%.0f, %.0f) eye1 (%.0f, %.0f) px; "
                    "fit %s n %d delay %.0f ms resid %.1f px, x %.0f*t%+.0f / %.0f*t%+.0f, y %.0f*t%+.0f / %.0f*t%+.0f",
                    hooked, (unsigned long long)calls, (unsigned long long)vcalls, src, cx[0], cy[0], cx[1], cy[1],
                    calibrated ? "ok" : "default", fit.n, fit.delay_ms, fit.resid_px, fit.ax[0], fit.bx[0], fit.ax[1], fit.bx[1],
                    fit.ay[0], fit.by[0], fit.ay[1], fit.by[1]);
        }
    }
    // v33: lens centre = each eye's straight-ahead (calibrated fit offsets), else the tile centre
    float lcx[2] = {0.5f * T, 0.5f * T}, lcy[2] = {0.5f * T, 0.5f * T};
    if (fit.valid) for (int e = 0; e < 2; ++e) { lcx[e] = fit.bx[e]; lcy[e] = fit.by[e]; }
    float pxpd = T / s.fov;
    // v35: hide box = intersection of the inset squares centred on the gaze over [delay - w, delay + w], plus now.
    float hb[2][4] = {};
    {
        float half = scale * T * 0.5f;
        float mnx[2] = {1e9f, 1e9f}, mxx[2] = {-1e9f, -1e9f}, mny[2] = {1e9f, 1e9f}, mxy[2] = {-1e9f, -1e9f};
        bool any = false;
        if (valid && confident && s.gaze) {
            for (int k = -4; k <= 5; ++k) {
                double t = (k == 5) ? now : now - (delay + k * s.hide_window_ms / 4.0f) * 1e-3;
                SlpGaze gs;
                if (!slp_gaze_at(t, &gs) || now - gs.t > s.gaze_max_age_ms * 1e-3) continue;
                float tx = gs.x / -gs.z, ty = gs.y / -gs.z;
                for (int e = 0; e < 2; ++e) {
                    float x = fit.ax[e] * tx + fit.bx[e], y = fit.ay[e] * ty + fit.by[e];
                    mnx[e] = std::min(mnx[e], x); mxx[e] = std::max(mxx[e], x);
                    mny[e] = std::min(mny[e], y); mxy[e] = std::max(mxy[e], y);
                }
                any = true;
            }
        } else if (valid) {   // locator only
            for (int e = 0; e < 2; ++e) { mnx[e] = mxx[e] = cx[e]; mny[e] = mxy[e] = cy[e]; }
            any = true;
        }
        for (int e = 0; e < 2 && any; ++e) {
            hb[e][0] = 0.5f * (mnx[e] + mxx[e]);
            hb[e][1] = 0.5f * (mny[e] + mxy[e]);
            hb[e][2] = std::max(0.0f, half - 0.5f * (mxx[e] - mnx[e]));
            hb[e][3] = std::max(0.0f, half - 0.5f * (mxy[e] - mny[e]));
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_box_mu);
        for (int e = 0; e < 2; ++e)
            for (int i = 0; i < 4; ++i) g_box[e][i] = T > 0 ? hb[e][i] / T : 0.0f;
    }
    float params[60] = {
        T, s.fov, scale, valid ? 1.0f : 0.0f,
        cx[0], cy[0], cx[1], cy[1],
        s.safe, s.ramp, s.cpd0, s.e2,
        s.strength, s.max_lod, s.contrast, s.fovea_blur ? 1.0f : 0.0f,
        (s.hide && confident) ? 1.0f : 0.0f, s.hide_margin, s.hide_lod, s.safe_unlocated,
        s.wide_min_lod, now_ok ? 1.0f : 0.0f, s.chroma_luma ? 1.0f : 0.0f, 0.0f,
        nx[0], ny[0], nx[1], ny[1],
        lcx[0], lcy[0], lcx[1], lcy[1],
        s.lens_deg * pxpd, s.lens_feather_deg * pxpd, s.chroma_lod, s.contrast_clamp ? 1.0f : 0.0f,
        hb[0][0], hb[0][1], hb[0][2], hb[0][3],
        hb[1][0], hb[1][1], hb[1][2], hb[1][3],
        fit.ax[0], fit.bx[0], fit.ay[0], fit.by[0],
        fit.ax[1], fit.bx[1], fit.ay[1], fit.by[1],
        s.edge_ramp, s.edge_start, std::max(s.edge_end, s.edge_start + 1.0f), s.true_angles ? 1.0f : 0.0f,
        s.contrast_overshoot, s.temporal,
        // history only if it is last frame's (no gap, same size)
        (g_r.hist_frame != 0 && g_r.hist_frame + 1 == g_r.frame) ? 1.0f : 0.0f, s.temporal_clamp,
    };
    // v45: what the Frame and the bit weights need to follow the same ramp (inset centres + projection, tile units)
    {
        SlpRampInfo ri;
        ri.valid = valid;
        for (int e = 0; e < 2; ++e) {
            ri.cx[e] = valid ? cx[e] / T : 0.5f; ri.cy[e] = valid ? cy[e] / T : 0.5f;
            ri.ax[e] = fit.ax[e] / T; ri.bx[e] = fit.bx[e] / T; ri.ay[e] = fit.ay[e] / T; ri.by[e] = fit.by[e] / T;
        }
        ri.ramp = s.edge_ramp; ri.start = s.edge_start; ri.end = std::max(s.edge_end, s.edge_start + 1.0f);
        ri.overshoot = s.contrast_overshoot;
        std::lock_guard<std::mutex> lk(g_box_mu);
        g_ramp = ri;
    }
    D3D11_MAPPED_SUBRESOURCE mcb;
    if (FAILED(ctx->lpVtbl->Map(ctx, (ID3D11Resource *)g_r.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mcb))) return false;
    memcpy(mcb.pData, params, sizeof(params));
    ctx->lpVtbl->Unmap(ctx, (ID3D11Resource *)g_r.cb, 0);

    // Save vrlink's compute state, run ours, restore.
    ID3D11ComputeShader *old_cs = nullptr;
    ID3D11ClassInstance *old_ci[16] = {};
    UINT old_nci = 16;
    ID3D11ShaderResourceView *old_srv = nullptr, *old_srv1 = nullptr;
    ID3D11UnorderedAccessView *old_uav = nullptr;
    ID3D11Buffer *old_cb = nullptr;
    ID3D11SamplerState *old_samp = nullptr;
    ctx->lpVtbl->CSGetShader(ctx, &old_cs, old_ci, &old_nci);
    ctx->lpVtbl->CSGetShaderResources(ctx, 0, 1, &old_srv);
    ctx->lpVtbl->CSGetShaderResources(ctx, 1, 1, &old_srv1);
    ctx->lpVtbl->CSGetUnorderedAccessViews(ctx, 0, 1, &old_uav);
    ctx->lpVtbl->CSGetConstantBuffers(ctx, 0, 1, &old_cb);
    ctx->lpVtbl->CSGetSamplers(ctx, 0, 1, &old_samp);

    // v46: Gaussian pyramid, level by level (each pass reads level k - 1 and writes level k)
    ctx->lpVtbl->CSSetShader(ctx, g_r.pyr_cs, nullptr, 0);
    {
        ID3D11ShaderResourceView *null_srv = nullptr;
        ID3D11UnorderedAccessView *null_uav = nullptr;
        for (UINT k = 1; k < (UINT)(kLocMip + 3); ++k) {
            UINT w = std::max(1u, ad.Width >> k), h = std::max(1u, ad.Height >> k);
            ctx->lpVtbl->CSSetShaderResources(ctx, 0, 1, &g_r.lvl_srv[k - 1]);
            ctx->lpVtbl->CSSetUnorderedAccessViews(ctx, 0, 1, &g_r.lvl_uav[k], nullptr);
            ctx->lpVtbl->Dispatch(ctx, (w + 7) / 8, (h + 7) / 8, 1);
            ctx->lpVtbl->CSSetUnorderedAccessViews(ctx, 0, 1, &null_uav, nullptr);
            ctx->lpVtbl->CSSetShaderResources(ctx, 0, 1, &null_srv);
        }
    }
    ctx->lpVtbl->CSSetShader(ctx, g_r.cs, nullptr, 0);
    ctx->lpVtbl->CSSetShaderResources(ctx, 0, 1, &g_r.mip_srv);
    ctx->lpVtbl->CSSetShaderResources(ctx, 1, 1, &g_r.hist_srv);   // v47
    ctx->lpVtbl->CSSetUnorderedAccessViews(ctx, 0, 1, &g_r.out_uav, nullptr);
    ctx->lpVtbl->CSSetConstantBuffers(ctx, 0, 1, &g_r.cb);
    ctx->lpVtbl->CSSetSamplers(ctx, 0, 1, &g_r.samp);
    ctx->lpVtbl->Dispatch(ctx, (ad.Width + 7) / 8, (ad.Height + 7) / 8, 1);

    ID3D11UnorderedAccessView *null_uav = nullptr;
    ctx->lpVtbl->CSSetUnorderedAccessViews(ctx, 0, 1, &null_uav, nullptr);
    ctx->lpVtbl->CSSetShader(ctx, old_cs, old_ci, old_nci);
    ctx->lpVtbl->CSSetShaderResources(ctx, 0, 1, &old_srv);
    ctx->lpVtbl->CSSetShaderResources(ctx, 1, 1, &old_srv1);
    ctx->lpVtbl->CSSetUnorderedAccessViews(ctx, 0, 1, &old_uav, nullptr);
    ctx->lpVtbl->CSSetConstantBuffers(ctx, 0, 1, &old_cb);
    ctx->lpVtbl->CSSetSamplers(ctx, 0, 1, &old_samp);
    rel(old_cs); rel(old_srv); rel(old_srv1); rel(old_uav); rel(old_cb); rel(old_samp);
    for (UINT i = 0; i < old_nci && i < 16; ++i) rel(old_ci[i]);

    if (s.locator) readback_for_locator(ctx);   // v46: after the pyramid (reads its level kLocMip)
    if (s.temporal > 0.0f) {   // v47: keep this output for next frame's stability blend
        ctx->lpVtbl->CopyResource(ctx, (ID3D11Resource *)g_r.hist, (ID3D11Resource *)g_r.out);
        g_r.hist_frame = g_r.frame;
    } else {
        g_r.hist_frame = 0;
    }
    ctx->lpVtbl->CopyResource(ctx, (ID3D11Resource *)dst, (ID3D11Resource *)g_r.out);
    return true;
}

void slp_periph_csf(unsigned tile_px, float *nyq_fovea, float *nyq_wide, float *nyq_display)
{
    load_settings();
    Settings s = settings();
    *nyq_fovea = *nyq_wide = *nyq_display = 0.0f;
    if (!s.csf || tile_px == 0) return;
    Estimate est = s.locator ? estimate() : Estimate{};
    float scale = s.inset_scale > 0 ? s.inset_scale : (est.valid ? est.scale : 0.29f);
    *nyq_wide = 0.5f * (float)tile_px / s.fov;
    *nyq_fovea = *nyq_wide / std::max(0.05f, scale);
    *nyq_display = s.display_cpd;
}

void slp_periph_inset_boxes(float box[2][4])
{
    std::lock_guard<std::mutex> lk(g_box_mu);
    memcpy(box, g_box, sizeof(g_box));
}

SlpRampInfo slp_periph_ramp()
{
    std::lock_guard<std::mutex> lk(g_box_mu);
    return g_ramp;
}
