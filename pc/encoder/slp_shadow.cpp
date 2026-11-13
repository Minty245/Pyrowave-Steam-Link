// Steam Link Pyrowave - the Pyrowave encoder that stands in for vrlink's HEVC (PC side).
// SPDX-License-Identifier: MIT
//
// vrlink renders its foveated atlas and hands it to NVENC. In substitution mode (settings.json general.enabled = 1,
// the default) slp_nvenc_hook.cpp answers vrlink's NVENC calls itself (NVENC is bypassed, v18; vrlink's pacing and
// stats see a normal encoder) and this file encodes the atlas with Pyrowave into an SLPW frame:
//
//   vrlink encoder thread (inside the NvEncEncodePicture hook):
//     peripheral prefilter (slp_periph.cpp) -> CopyResource(atlas -> our shared slot)
//     ID3D11DeviceContext4::Signal(shared fence, ++v), Flush, hand {slot, v} to the worker
//   worker thread (Vulkan, high-priority compute queue):
//     pyrowave_encoder_encode_gpu_scaled (waits for the fence; RGB -> BT.709 full-range YCbCr 4:2:0, or 4:4:4 with
//     general.full_colour), region weights + per-tile CSF (Pyrowave fork), packetize -> SLPW frame
//   NvEncLockBitstream: returns that frame (slp_shadow_take_frame).
//
// Budget: vrlink's live encoder target x general.multiplier, with the safety brake after a lost frame (v51).
// Safety: any init failure turns substitution off for the session (plain HEVC); the encoder thread never blocks
// for long. Debug switches (files in %TEMP%): slp_shadow_off (kill switch), slp_prio_medium, slp_d3d_prio,
// slp_shadow_measure (measure-only shadow encoding with substitution off).

#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <deque>
#include <mutex>
#include <vector>

#include <vulkan/vulkan.h>
#include "pyrowave.h"

#include "slp_shadow.h"
#include "slp_settings.h"
#include "slp_status.h"
#include "slp_periph.h"

// v45: the edge ramp's start and end angles as distances from the inset centre in the wide view (tile fractions),
// averaged over 8 directions (the angle per pixel varies across a flat projection; the bit weights only need a
// round approximation).
static void slp_ramp_radii(const SlpRampInfo &r, int e, float *start, float *end)
{
    auto ang = [&](float x, float y) {
        float ax = (x - r.bx[e]) / r.ax[e], ay = (y - r.by[e]) / r.ay[e];
        float cx = (r.cx[e] - r.bx[e]) / r.ax[e], cy = (r.cy[e] - r.by[e]) / r.ay[e];
        float d = ax * cx + ay * cy + 1.0f;
        float c0 = ay - cy, c1 = cx - ax, c2 = ax * cy - ay * cx;   // |(ax, ay, 1) x (cx, cy, 1)|
        return std::atan2(std::sqrt(c0 * c0 + c1 * c1 + c2 * c2), d) * 57.29578f;
    };
    float sum[2] = {0, 0};
    for (int k = 0; k < 8; ++k) {
        float dx = std::cos(k * 0.785398f), dy = std::sin(k * 0.785398f);
        for (int w = 0; w < 2; ++w) {
            float target = w ? r.end : r.start, lo = 0.0f, hi = 0.75f;   // directions leaving the view count as 0.75
            for (int it = 0; it < 24; ++it) {
                float mid = 0.5f * (lo + hi);
                (ang(r.cx[e] + dx * mid, r.cy[e] + dy * mid) < target ? lo : hi) = mid;
            }
            sum[w] += 0.5f * (lo + hi);
        }
    }
    *start = sum[0] / 8.0f;
    *end = std::max(sum[1] / 8.0f, *start + 0.01f);
}

void push_result(uint64_t index, std::vector<uint8_t> bytes);

void slp_apply_region_weights();

namespace {

// GUIDs copied from the MinGW SDK headers (verified by script at build time).
const GUID kIID_ID3D11Device5        = {0x8ffde202, 0xa0e7, 0x45df, {0x9e, 0x01, 0xe8, 0x37, 0x80, 0x1b, 0x5e, 0xa0}};
const GUID kIID_ID3D11DeviceContext4 = {0x917600da, 0xf58c, 0x4c33, {0x98, 0xd8, 0x3e, 0x15, 0xb3, 0x90, 0xfa, 0x24}};
const GUID kIID_ID3D11Fence          = {0xaffde9d1, 0x1df7, 0x4bb7, {0x8a, 0x34, 0x0f, 0x46, 0x25, 0x1d, 0xab, 0x80}};
const GUID kIID_IDXGIResource1       = {0x30961379, 0x4609, 0x4a41, {0x99, 0x8e, 0x54, 0xfe, 0x56, 0x7e, 0xe0, 0xc1}};
const GUID kIID_IDXGIDevice          = {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};

constexpr int      kSlots         = 3;
constexpr uint64_t kStartAfter    = 240;   // let the stream settle first (~1.7 s at 144 Hz)
// v17 substitution mode (%TEMP%\slp_substitute present when the stream starts): Pyrowave 4:2:0
// frames replace NVENC's HEVC bytes in NvEncLockBitstream. Starts on the first frame; the
// per-frame budget follows vrlink's own average bitrate x multiplier (%TEMP%\slp_multiplier.txt).
bool g_substitute = false;
// v50: full colour (4:4:4) in substitution mode, settings.json general.full_colour; read when the encoder is created
// (stream start / resize). Needs full colour armed on the Frame (layer v7.19).
static bool g_c444 = false;
double g_multiplier = 1.0;
// v19: the budget follows vrlink's live encoder target (NvEncReconfigureEncoder average). That target
// already includes vrlink's FEC headroom and its loss back-off (driver_vrlink scales the setting by two
// live factors), so 1.0 x it is what vrlink's pacer expects. Above 1.0 the safety brake below decides.
// Brake (v51, see slp_substitute_brake): a keyframe request / encoder reset / sharp target cut from vrlink
// trims only the multiplier's excess above 1.0, then recovers linearly within ~0.3 s.
std::atomic<double>   g_eff_mult{1.0};
std::atomic<int64_t>  g_brake_until_ms{0};
std::atomic<uint64_t> g_brakes{0};
std::atomic<double> g_frame_ms{1000.0 / 144.0};   // EMA of the EncodePicture interval
// NVENC async mode (bypass): completion event to signal once frame `index` has a result.
struct PendingEvent { uint64_t index; void *event; };
PendingEvent g_events[32];
struct SubResult { uint64_t index; std::vector<uint8_t> bytes; };
std::mutex              g_res_mutex;
std::condition_variable g_res_cv;
std::deque<SubResult>   g_results;          // newest at the back, at most 8
constexpr int      kBlockFrames   = 720;   // stats block (~5 s)
constexpr double   kStreamFps     = 144.0; // measured in rounds 4-4d (143.5-144.0)
// Rotating per-frame byte budgets, one per stats block, so a single run shows
// cost and size at several rates.  Design ceiling is 500 Mbps.
constexpr int      kBudgetsMbps[] = {200, 350, 500};

enum State : int { kOff = 0, kStarting, kReady, kDisabled, kResizing };
// v25 resize handshake (atlas size changed at a stream restart): 0 idle, 1 worker released the Vulkan side,
// 2 encoder thread made new D3D11 slots (worker re-imports them).
std::atomic<int> g_resize_stage{0};
std::atomic<bool> g_regions_dirty{true};   // re-apply weights / CSF to a (new) encoder

struct Slot {
    ID3D11Texture2D   *tex = nullptr;
    pyrowave_image     img = nullptr;
    pyrowave_image_view view = {};
    std::atomic<bool>  busy{false};
};

struct Job {
    int      slot;
    uint64_t fence_value;
    uint64_t encode_index;
    std::chrono::steady_clock::time_point t_submit;
    bool     sample;      // v11: this frame also gets a quality / sync check
};

// v11 quality sampling: every kSampleEvery encodes, the encoder thread also queues
// GPU copies of (a) the slot Pyrowave reads and (b) the P010 texture NVENC gets for
// the same EncodePicture into CPU-readable staging textures.  The worker decodes the
// Pyrowave bitstream on the PC and compares.  Nothing leaves the PC; only numbers
// are logged.
// v13 showed each sample delays that frame's NVENC encode by ~14 ms (18-22 ms vs ~4 ms):
// the ~80 MB of copies into CPU-readable staging go onto vrlink's queue right before
// NVENC reads its input.  Sample rarely (~every 10 s at 144 Hz); the encode itself costs
// NVENC nothing measurable.
constexpr uint64_t kSampleEvery  = 1440;   // ~10 s at 144 fps
constexpr uint64_t kSampleOffset = 720;
ID3D11DeviceContext   *g_ctx = nullptr;          // vrlink's immediate context (borrowed)
ID3D11Texture2D       *g_stage_rgba = nullptr;   // RGBA16F, CPU read
ID3D11Texture2D       *g_stage_p010 = nullptr;   // P010, CPU read
UINT                   g_p010_w = 0, g_p010_h = 0; // size of g_stage_p010 (follows vrlink's encode size)
std::atomic<bool>      g_sample_busy{false};
pyrowave_decoder       g_pdec = nullptr;

std::atomic<int>       g_state{kOff};
Slot                   g_slots[kSlots];
int                    g_next_slot = 0;          // encoder thread only
ID3D11DeviceContext4  *g_ctx4 = nullptr;
ID3D11Fence           *g_fence = nullptr;
uint64_t               g_fence_value = 0;        // encoder thread only
UINT                   g_width = 0, g_height = 0;

// Handed from the encoder thread to the worker for Vulkan-side import.
HANDLE                 g_slot_handles[kSlots] = {};
HANDLE                 g_fence_handle = nullptr;
LUID                   g_luid = {};
UINT                   g_vendor = 0, g_device_id = 0;

pyrowave_device        g_pdev = nullptr;
pyrowave_encoder       g_penc = nullptr;
pyrowave_sync_object   g_psync = nullptr;

std::mutex              g_q_mutex;
std::condition_variable g_q_cv;
std::deque<Job>         g_queue;

// Stats (worker thread writes, encoder thread only reads the drop counter).
std::atomic<uint64_t> g_dropped{0};
std::atomic<uint64_t> g_submitted{0};
std::atomic<uint64_t> g_samples{0};

// v13 isolation switches, read once when the shadow encoder starts (files in %TEMP%):
//   slp_shadow_sample    - v16: quality sampling is OFF by default (its staging copies delay vrlink's
//                          NVENC frame by ~15 ms); create this file to turn it back on
//   slp_shadow_noencode  - D3D11 copy + fence signal only; Pyrowave encode skipped
//   slp_shadow_noflush   - do not Flush vrlink's context after our copy/signal
bool g_opt_nosample = false, g_opt_noencode = false, g_opt_noflush = false;

struct BlockStats {
    uint64_t frames = 0, failed = 0;
    uint64_t bytes = 0, bytes_max = 0;
    double   lat_sum_ms = 0, lat_max_ms = 0;     // submit (encoder thread) -> bitstream ready
    double   call_sum_ms = 0;                    // CPU time inside encode_gpu_scaled
    double   d3d_sum_ms = 0, d3d_max_ms = 0;     // v31: submit -> vrlink's D3D11 work (prefilter + copy) done
    uint64_t dropped_at_start = 0;
};

void pyro_msg(void *, const char *msg)
{
    // Pyrowave's perf report lines; strip trailing newline.
    char buf[400];
    std::snprintf(buf, sizeof(buf), "%s", msg ? msg : "");
    size_t n = std::strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    if (n) slp_log("[PYRO] %s", buf);
}

template <typename T> void safe_release(T *&p)
{
    if (p) { p->lpVtbl->Release(p); p = nullptr; }
}

void disable(const char *why)
{
    g_state.store(kDisabled);
    slp_log("[SHADOW] disabled: %s (NVENC stream unaffected)", why);
}

// ---------------------------------------------------------------------------
// Worker thread: Pyrowave device + encoder + imports, then the encode loop.
// ---------------------------------------------------------------------------

bool create_encoder_and_slots();
void destroy_encoder_and_slots();

bool worker_init()
{
    uint32_t maj = 0, min = 0, pat = 0;
    pyrowave_get_api_version(&maj, &min, &pat);
    slp_log("[SHADOW] Pyrowave API %u.%u.%u; creating Vulkan device by LUID %08lx:%08lx (vendor 0x%04x device 0x%04x)",
            maj, min, pat, (unsigned long)g_luid.HighPart, (unsigned long)g_luid.LowPart, g_vendor, g_device_id);

    pyrowave_luid pl = {};
    static_assert(sizeof(LUID) == sizeof(pl.luid), "LUID size");
    std::memcpy(pl.luid, &g_luid, sizeof(LUID));

    auto t0 = std::chrono::steady_clock::now();
    // v31: high-priority compute queue so a GPU-bound game does not hold up the encode (NVENC has its own engine;
    // Pyrowave shares the shader cores). Falls back to medium if the driver refuses. Kill switch: slp_prio_medium.
    VkQueueGlobalPriority prio = temp_flag_exists("slp_prio_medium") ? VK_QUEUE_GLOBAL_PRIORITY_MEDIUM : VK_QUEUE_GLOBAL_PRIORITY_HIGH;
    pyrowave_result r = pyrowave_create_device_by_compat(g_vendor, g_device_id, nullptr, nullptr, &pl, prio, &g_pdev);
    if ((r != PYROWAVE_SUCCESS || !g_pdev) && prio != VK_QUEUE_GLOBAL_PRIORITY_MEDIUM) {
        slp_log("[SHADOW] high-priority device failed (%d); retrying at medium priority", (int)r);
        prio = VK_QUEUE_GLOBAL_PRIORITY_MEDIUM;
        r = pyrowave_create_device_by_compat(g_vendor, g_device_id, nullptr, nullptr, &pl, prio, &g_pdev);
    }
    slp_log("[SHADOW] requested queue priority %s", prio == VK_QUEUE_GLOBAL_PRIORITY_HIGH ? "HIGH" : "MEDIUM");
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (r != PYROWAVE_SUCCESS || !g_pdev) {
        slp_log("[SHADOW] pyrowave_create_device_by_compat failed: %d (%.0f ms)", (int)r, ms);
        return false;
    }
    slp_log("[SHADOW] Pyrowave device created in %.0f ms; interop support: %s", ms,
            pyrowave_device_confirm_interop_support(g_pdev) ? "yes" : "NO");

    // Async compute keeps Pyrowave off the graphics queue the game/compositor use.
    r = pyrowave_device_set_queue_type(g_pdev, VK_QUEUE_COMPUTE_BIT);
    slp_log("[SHADOW] queue type COMPUTE: %d", (int)r);

    if (!create_encoder_and_slots()) return false;

    pyrowave_sync_object_create_info sci = {};
    sci.device = g_pdev;
    sci.external_handle = (pyrowave_os_handle)g_fence_handle;   // ownership passes to Pyrowave
    sci.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;  // D3D11 fence == D3D12 fence
    sci.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;
    r = pyrowave_sync_object_create(&sci, &g_psync);
    g_fence_handle = nullptr;
    if (r != PYROWAVE_SUCCESS || !g_psync) {
        slp_log("[SHADOW] fence import failed: %d", (int)r);
        return false;
    }
    return true;
}

// v50: switch the substitute stream between 4:2:0 and full colour (4:4:4) live, between two frames (the encoder is
// idle then: packetize waited for the GPU). Only the encoder is recreated; the slots stay.
static void switch_chroma(bool c444)
{
    if (!g_penc || c444 == g_c444) return;
    pyrowave_encoder_destroy(g_penc);
    g_penc = nullptr;
    for (int attempt = 0; attempt < 2 && !g_penc; ++attempt) {
        const bool want = attempt == 0 ? c444 : !c444;
        pyrowave_encoder_create_info eci = {};
        eci.device = g_pdev;
        eci.width = (int)g_width;
        eci.height = (int)g_height;
        eci.chroma = want ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
        if (pyrowave_encoder_create(&eci, &g_penc) == PYROWAVE_SUCCESS && g_penc) g_c444 = want;
        else g_penc = nullptr;
    }
    g_regions_dirty.store(true);   // weights / CSF / ramp go to the new encoder
    if (g_penc)
        slp_log("[SUB] %s (settings.json general.full_colour)%s", g_c444 ? "FULL COLOUR on: encoding 4:4:4" : "full colour off: encoding 4:2:0",
                g_c444 ? " - the Frame must have full colour armed (layer v7.19+), or it shows no picture" : "");
    else
        slp_log("[SUB] encoder re-creation for the chroma switch failed; frames will repeat");
}

// Encoder + Vulkan imports of the D3D11 slot textures, at g_width x g_height (also used after a resize).
bool create_encoder_and_slots()
{
    pyrowave_result r;
    std::chrono::steady_clock::time_point t0;
    double ms;
    pyrowave_encoder_create_info eci = {};
    eci.device = g_pdev;
    eci.width = (int)g_width;
    eci.height = (int)g_height;
    g_c444 = !g_substitute || slp_setting("general.full_colour", 0) != 0;
    eci.chroma = g_c444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
    t0 = std::chrono::steady_clock::now();
    r = pyrowave_encoder_create(&eci, &g_penc);
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (r != PYROWAVE_SUCCESS || !g_penc) {
        slp_log("[SHADOW] pyrowave_encoder_create %ux%u %s failed: %d", g_width, g_height, g_c444 ? "4:4:4" : "4:2:0", (int)r);
        return false;
    }
    slp_log("[SHADOW] encoder %ux%u %s created in %.0f ms%s", g_width, g_height, g_c444 ? "4:4:4" : "4:2:0", ms,
            g_substitute && g_c444 ? " (FULL COLOUR: the Frame needs full colour armed, layer v7.19+)" : "");

    for (int i = 0; i < kSlots; ++i) {
        VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R16G16B16A16_SFLOAT;     // DXGI_FORMAT_R16G16B16A16_FLOAT
        ici.extent = {g_width, g_height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        pyrowave_image_create_info info = {};
        info.device = g_pdev;
        info.external_handle = (pyrowave_os_handle)g_slot_handles[i];   // ownership passes to Pyrowave
        info.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        info.image_create_info = &ici;
        r = pyrowave_image_create(&info, &g_slots[i].img);
        g_slot_handles[i] = nullptr;
        if (r != PYROWAVE_SUCCESS || !g_slots[i].img) {
            slp_log("[SHADOW] import of slot %d (D3D11 NT handle, RGBA16F) failed: %d", i, (int)r);
            return false;
        }
        r = pyrowave_image_get_image_view(g_slots[i].img, VK_IMAGE_ASPECT_COLOR_BIT,
                                          VK_IMAGE_USAGE_SAMPLED_BIT, &g_slots[i].view);
        if (r != PYROWAVE_SUCCESS) {
            // Fall back to a hand-built view: same format, colour aspect, GENERAL layout.
            slp_log("[SHADOW] pyrowave_image_get_image_view slot %d returned %d; using manual view", i, (int)r);
            pyrowave_image_view v = {};
            v.image = pyrowave_image_get_handle(g_slots[i].img);
            v.width = g_width;
            v.height = g_height;
            v.image_format = VK_FORMAT_R16G16B16A16_SFLOAT;
            v.view_format = VK_FORMAT_R16G16B16A16_SFLOAT;
            v.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            v.swizzle = VK_COMPONENT_SWIZZLE_IDENTITY;
            v.layout = VK_IMAGE_LAYOUT_GENERAL;
            g_slots[i].view = v;
        }
    }
    return true;
}

void destroy_encoder_and_slots()
{
    if (g_penc) { pyrowave_encoder_destroy(g_penc); g_penc = nullptr; }
    for (int i = 0; i < kSlots; ++i)
        if (g_slots[i].img) { pyrowave_image_destroy(g_slots[i].img); g_slots[i].img = nullptr; g_slots[i].view = {}; }
    if (g_pdec) { pyrowave_decoder_destroy(g_pdec); g_pdec = nullptr; }
}

// IEEE half -> float via table (bit-exact, no F16C dependency).
std::vector<float> g_half_lut;
void init_half_lut()
{
    g_half_lut.resize(65536);
    for (uint32_t h = 0; h < 65536; ++h) {
        uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1f, man = h & 0x3ff, f;
        if (exp == 0) {
            if (man == 0) f = sign;
            else {                       // subnormal
                exp = 127 - 15 + 1;
                while (!(man & 0x400)) { man <<= 1; --exp; }
                man &= 0x3ff;
                f = sign | (exp << 23) | (man << 13);
            }
        } else if (exp == 31) f = sign | 0x7f800000u | (man << 13);
        else f = sign | ((exp + 127 - 15) << 23) | (man << 13);
        float v;
        std::memcpy(&v, &f, 4);
        g_half_lut[h] = v;
    }
}

// Map a staging texture without ever blocking inside D3D11 (vrlink's render thread
// shares the multithread lock): poll with DO_NOT_WAIT.
bool map_staging_polled(ID3D11Texture2D *t, D3D11_MAPPED_SUBRESOURCE *ms, int timeout_ms)
{
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        HRESULT hr = g_ctx->lpVtbl->Map(g_ctx, (ID3D11Resource *)t, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, ms);
        if (SUCCEEDED(hr)) return true;
        if (hr != DXGI_ERROR_WAS_STILL_DRAWING) { slp_log("[QUALITY] Map failed 0x%08lx", (unsigned long)hr); return false; }
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(timeout_ms)) {
            slp_log("[QUALITY] Map timed out"); return false;
        }
        Sleep(1);
    }
}

inline double psnr(double sse, double n) { return sse <= 0 ? 99.0 : 10.0 * std::log10(n / sse); }

// v22: works in substitution (4:2:0) and shadow (4:4:4) mode. Logs luma PSNR per atlas tile and in the
// centre of the foveal insets (to see what the region weights do), and, when vrlink's encode size differs
// from the atlas, how its NVENC input maps onto the atlas (crop / per-tile crop / scale).
void analyze_sample(const Job &job, int budget_mbps, const uint8_t *bits, const std::vector<pyrowave_packet> &packets,
                    size_t num_packets, size_t frame_bytes)
{
    const int W = (int)g_width, H = (int)g_height;
    const size_t N = (size_t)W * H;
    const bool c420 = !g_c444;
    const int CW = c420 ? W / 2 : W, CH = c420 ? H / 2 : H;
    const size_t CN = (size_t)CW * CH;
    auto t0 = std::chrono::steady_clock::now();

    // 1. Decode our own bitstream on the PC (same device, compute path, 16-bit output).
    static bool dec420 = false;
    if (g_pdec && dec420 != c420) { pyrowave_decoder_destroy(g_pdec); g_pdec = nullptr; }
    if (!g_pdec) {
        pyrowave_decoder_create_info dci = {};
        dci.device = g_pdev; dci.width = W; dci.height = H;
        dci.chroma = c420 ? PYROWAVE_CHROMA_SUBSAMPLING_420 : PYROWAVE_CHROMA_SUBSAMPLING_444; dci.fragment_path = false;
        if (pyrowave_decoder_create(&dci, &g_pdec) != PYROWAVE_SUCCESS) { slp_log("[QUALITY] decoder create failed"); return; }
        dec420 = c420;
        init_half_lut();
    }
    static std::vector<uint16_t> dec[3];
    dec[0].resize(N); dec[1].resize(CN); dec[2].resize(CN);
    pyrowave_cpu_buffer cb = {};
    for (int i = 0; i < 3; ++i) {
        cb.data[i] = dec[i].data();
        cb.row_stride_in_bytes[i] = (size_t)(i ? CW : W) * 2;
        cb.plane_size_in_bytes[i] = (i ? CN : N) * 2;
    }
    cb.width = W; cb.height = H;
    cb.format = c420 ? PYROWAVE_CPU_BUFFER_FORMAT_YUV420P16 : PYROWAVE_CPU_BUFFER_FORMAT_YUV444P16;
    // Start every sample from a clean decoder (the bitstream's wrapping sequence counter, v12 fix).
    pyrowave_decoder_clear(g_pdec);
    for (size_t i = 0; i < num_packets; ++i)
        pyrowave_decoder_push_packet(g_pdec, bits + packets[i].offset, packets[i].size);
    bool complete = pyrowave_decoder_decode_is_ready(g_pdec, false);
    pyrowave_result r = pyrowave_decoder_decode_cpu_buffer_synchronous(g_pdec, &cb);
    double dec_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (r != PYROWAVE_SUCCESS) { slp_log("[QUALITY] decode failed %d", (int)r); return; }

    // 2. Read back the exact input and NVENC's input for the same EncodePicture.
    D3D11_MAPPED_SUBRESOURCE ma = {}, mp = {};
    if (!map_staging_polled(g_stage_rgba, &ma, 250)) return;
    bool have_p010 = g_stage_p010 && map_staging_polled(g_stage_p010, &mp, 250);
    const int PW = (int)g_p010_w, PH = (int)g_p010_h;

    // 3. Reference luma + per-tile / fovea-centre luma PSNR. Chroma only for 4:4:4.
    static std::vector<float> ry, rcb, rcr;
    ry.resize(N);
    if (!c420) { rcb.resize(N); rcr.resize(N); }
    double sse[3] = {}, bias[3] = {};
    const int tiles = 4, tile_h = H / tiles;
    double tsse[4] = {}, tn[4] = {}, csse = 0, cn = 0;
    for (int y = 0; y < H; ++y) {
        auto row = reinterpret_cast<const uint16_t *>(static_cast<const uint8_t *>(ma.pData) + (size_t)y * ma.RowPitch);
        int tile = std::min(y / tile_h, tiles - 1);
        float ly = (float)(y - tile * tile_h) / tile_h - 0.5f;
        for (int x = 0; x < W; ++x) {
            float R = std::min(std::max(g_half_lut[row[4 * x + 0]], 0.0f), 1.0f);
            float G = std::min(std::max(g_half_lut[row[4 * x + 1]], 0.0f), 1.0f);
            float B = std::min(std::max(g_half_lut[row[4 * x + 2]], 0.0f), 1.0f);
            float Y = 0.2126f * R + 0.7152f * G + 0.0722f * B;
            size_t i = (size_t)y * W + x;
            ry[i] = Y;
            double d0 = dec[0][i] / 65535.0 - Y;
            sse[0] += d0 * d0; bias[0] += d0;
            tsse[tile] += d0 * d0; tn[tile] += 1;
            float lx = (float)x / W - 0.5f;
            if ((tile & 1) && lx * lx + ly * ly < 0.0625f) { csse += d0 * d0; cn += 1; }   // within 1/4 of the inset size
            if (!c420) {
                float Cb = (B - Y) / 1.8556f + 0.5f, Cr = (R - Y) / 1.5748f + 0.5f;
                rcb[i] = Cb; rcr[i] = Cr;
                double d1 = dec[1][i] / 65535.0 - Cb, d2 = dec[2][i] / 65535.0 - Cr;
                sse[1] += d1 * d1; sse[2] += d2 * d2; bias[1] += d1; bias[2] += d2;
            }
        }
    }
    // 4. vrlink's encode size vs the atlas: how does NVENC's input map onto the atlas?
    if (have_p010) {
        double mae[4] = {}, mn = 0;
        const int ptile = PH / 4;
        for (int y = 0; y < PH; y += 2) {
            auto prow = reinterpret_cast<const uint16_t *>(static_cast<const uint8_t *>(mp.pData) + (size_t)y * mp.RowPitch);
            int t = std::min(y / std::max(ptile, 1), 3), yt = y - t * ptile;
            for (int x = 0; x < PW; x += 2) {
                double v = prow[x] / 65535.0;
                // 0: same pixel (sizes equal or top-left crop), 1: per-tile crop (tiles keep the atlas pitch),
                // 2: scaled to the atlas size, 3: per-tile crop + centred.
                int xs = std::min(W - 1, (int)((x + 0.5) * W / PW)), ys = std::min(H - 1, (int)((y + 0.5) * H / PH));
                int y1 = t * tile_h + yt;
                int xo = (tile_h - ptile) / 2;
                mae[0] += std::fabs(v - ry[(size_t)std::min(y, H - 1) * W + std::min(x, W - 1)]);
                mae[1] += std::fabs(v - ry[(size_t)std::min(y1, H - 1) * W + std::min(x, W - 1)]);
                mae[2] += std::fabs(v - ry[(size_t)ys * W + xs]);
                mae[3] += std::fabs(v - ry[(size_t)std::min(y1 + xo, H - 1) * W + std::min(x + xo, W - 1)]);
                mn += 1;
            }
        }
        if (mn > 0)
            slp_log("[SYNC] frame #%llu NVENC input %dx%d vs atlas %dx%d, luma MAE: same-pixel %.4f, per-tile crop %.4f, "
                    "scaled %.4f, per-tile centred crop %.4f (lowest = how vrlink fills the atlas)",
                    (unsigned long long)job.encode_index, PW, PH, W, H, mae[0] / mn, mae[1] / mn, mae[2] / mn, mae[3] / mn);
    }
    g_ctx->lpVtbl->Unmap(g_ctx, (ID3D11Resource *)g_stage_rgba, 0);
    if (have_p010) g_ctx->lpVtbl->Unmap(g_ctx, (ID3D11Resource *)g_stage_p010, 0);
    double total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    slp_log("[QUALITY] frame #%llu @ %d Mbps (%zu B, %s, %s): luma PSNR %.2f dB; per tile: wide %.2f, fovea %.2f, "
            "wide %.2f, fovea %.2f; fovea centre %.2f dB%s; decode %.1f ms, analysis %.0f ms",
            (unsigned long long)job.encode_index, budget_mbps, frame_bytes, c420 ? "4:2:0" : "4:4:4",
            complete ? "complete" : "INCOMPLETE", psnr(sse[0], (double)N), psnr(tsse[0], tn[0]), psnr(tsse[1], tn[1]),
            psnr(tsse[2], tn[2]), psnr(tsse[3], tn[3]), psnr(csse, std::max(cn, 1.0)),
            c420 ? "" : " (4:4:4 chroma PSNR in the next line)", dec_ms, total_ms);
    if (!c420)
        slp_log("[QUALITY] frame #%llu chroma PSNR Cb %.2f Cr %.2f dB (bias %+.5f %+.5f %+.5f)",
                (unsigned long long)job.encode_index, psnr(sse[1], (double)N), psnr(sse[2], (double)N),
                bias[0] / N, bias[1] / N, bias[2] / N);
}

void log_block(int budget_mbps, const BlockStats &b)
{
    if (!b.frames) return;
    double avg_bytes = (double)b.bytes / (double)b.frames;
    uint64_t dropped = g_dropped.load() - b.dropped_at_start;
    slp_log("[SHADOW] budget %d Mbps (%zu B/frame cap): %llu frames, avg %.0f B (max %llu) = %.1f Mbps at %.0f fps; "
            "latency avg %.2f ms max %.2f ms (D3D11 side avg %.2f max %.2f, Vulkan side avg %.2f); encode call %.2f ms; "
            "failed %llu; dropped %llu",
            budget_mbps, (size_t)(budget_mbps * 1e6 / 8.0 / kStreamFps),
            (unsigned long long)b.frames, avg_bytes, (unsigned long long)b.bytes_max,
            avg_bytes * 8.0 * kStreamFps / 1e6, kStreamFps,
            b.lat_sum_ms / (double)b.frames, b.lat_max_ms, b.d3d_sum_ms / (double)b.frames, b.d3d_max_ms,
            (b.lat_sum_ms - b.d3d_sum_ms) / (double)b.frames, b.call_sum_ms / (double)b.frames,
            (unsigned long long)b.failed, (unsigned long long)dropped);
    // GPU timestamps from Pyrowave itself (per-stage), then reset.
    pyrowave_device_report_performance_stats(g_pdev, pyro_msg, nullptr, true);
}

DWORD WINAPI worker_main(LPVOID)
{
    if (!worker_init()) {
        disable("initialisation failed");
        return 0;
    }
    g_state.store(kReady);
    if (g_substitute)
        slp_log("[SHADOW] ready: SUBSTITUTION mode, Pyrowave %s replaces HEVC; budget = vrlink avg bitrate x %.2f, %d slots",
                g_c444 ? "4:4:4 (full colour)" : "4:2:0", g_multiplier, kSlots);
    else
        slp_log("[SHADOW] ready: 4:4:4 shadow encode of the RGB atlas, %d slots, budgets 200/350/500 Mbps rotating every %d frames",
                kSlots, kBlockFrames);

    std::vector<uint8_t> bitstream;
    BlockStats block;
    block.dropped_at_start = g_dropped.load();
    uint64_t done = 0;

    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(g_q_mutex);
            g_q_cv.wait(lk, [] {
                return !g_queue.empty() || g_state.load() == kDisabled || (g_state.load() == kResizing && g_resize_stage.load() != 1);
            });
            if (g_queue.empty() && g_state.load() == kResizing) {
                lk.unlock();
                if (g_resize_stage.load() == 0) {
                    destroy_encoder_and_slots();
                    g_resize_stage.store(1);          // encoder thread now replaces the D3D11 slots
                    slp_log("[SHADOW] resize: Vulkan side released");
                } else if (g_resize_stage.load() == 2) {
                    if (!create_encoder_and_slots()) { disable("resize failed"); break; }
                    g_regions_dirty.store(true);
                    g_resize_stage.store(0);
                    g_state.store(kReady);
                    slp_log("[SHADOW] resize: encoder %ux%u ready", g_width, g_height);
                }
                continue;
            }
            if (g_queue.empty()) break;   // disabled
            job = g_queue.front();
            g_queue.pop_front();
        }

        if (g_opt_noencode) {          // isolation test: D3D11 side only
            g_slots[job.slot].busy.store(false);
            if (job.sample) g_sample_busy.store(false);
            continue;
        }

        int budget_mbps = kBudgetsMbps[(done / kBlockFrames) % (sizeof(kBudgetsMbps) / sizeof(kBudgetsMbps[0]))];
        size_t budget = (size_t)(budget_mbps * 1e6 / 8.0 / kStreamFps);
        if (g_substitute) {
            double base = slp_vrlink_avg_bps();
            if (base < 10e6) base = 30e6;                      // before vrlink's first reconfigure
            {
                // v36: the multiplier is live from settings.json (general.multiplier)
                static uint32_t mgen = 0;
                uint32_t gen = slp_settings_generation();
                if (gen != mgen) {
                    mgen = gen;
                    double m = slp_setting("general.multiplier", 1.0);
                    if (m >= 0.25 && m <= 4.0 && m != g_multiplier) {
                        slp_log("[SUB] multiplier %.2f -> %.2f (settings.json)", g_multiplier, m);
                        g_multiplier = m;
                        g_eff_mult.store(std::min(g_eff_mult.load(), m));
                    }
                    switch_chroma(slp_setting("general.full_colour", 0) != 0);   // v50
                }
            }
            // Recover the effective multiplier towards the user's value while vrlink is calm.
            {
                static auto last = std::chrono::steady_clock::now();
                auto now = std::chrono::steady_clock::now();
                double dt = std::chrono::duration<double, std::milli>(now - last).count();
                // v51: linear recovery of the trimmed excess over general.brake_recovery_ms (default 300 ms),
                // checked every frame instead of every 100 ms.
                if (dt >= 5.0) {
                    last = now;
                    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                    if (now_ms > g_brake_until_ms.load()) {
                        double e = g_eff_mult.load();
                        double rec_ms = std::max(50.0, (double)slp_setting("general.brake_recovery_ms", 100));
                        double step = std::max(0.0, g_multiplier - 1.0) * dt / rec_ms + 0.0005;
                        g_eff_mult.store(std::min(g_multiplier, e + step));
                    }
                }
            }
            double mult = std::min(g_eff_mult.load(), g_multiplier);
            double bps = base * mult;
            double fps = 1000.0 / std::max(1.0, g_frame_ms.load());
            fps = std::min(240.0, std::max(30.0, fps));
            budget_mbps = (int)(bps / 1e6);
            budget = (size_t)(bps / 8.0 / fps);
            budget = std::min<size_t>(std::max<size_t>(budget, 20000), 4000000);   // FEC rejects > 4 MiB
            static uint64_t last_log;
            if (done - last_log >= 720 || done < 2) {
                last_log = done;
                slp_log("[SUB] budget: vrlink target %.0f Mbps x %.2f (user %.2f, brakes %llu), %.1f fps -> %zu B/frame = %.0f Mbps; "
                        "%llu single-frame target dips >15%% since last line (ignored)",
                        base / 1e6, mult, g_multiplier, (unsigned long long)g_brakes.load(), fps, budget, bps / 1e6,
                        (unsigned long long)slp_vrlink_dips_take());
            }
        }
        if (bitstream.size() < budget) bitstream.resize(budget);

        Slot &s = g_slots[job.slot];
        pyrowave_gpu_external_reference ref = {s.img, VK_QUEUE_FAMILY_EXTERNAL};
        pyrowave_gpu_sync_operation acquire = {};
        acquire.images = &ref;
        acquire.num_images = 1;
        acquire.sync.semaphore = pyrowave_sync_object_get_semaphore(g_psync);
        acquire.sync.value = job.fence_value;
        pyrowave_gpu_sync_operation release = {};
        release.images = &ref;          // hand the image back to D3D11's queue family
        release.num_images = 1;

        pyrowave_scaled_encode_info si = {};
        si.view = s.view;
        si.input_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        si.output_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        si.intermediate_plane_format = VK_FORMAT_R16_UNORM;   // keep 16-bit precision like vrlink's P010
        si.ycbcr_chroma_midpoint = 0.5f;                      // vrlink: Cb/Cr = x + 0.5 (round 4d)
        si.skip_dither = true;
        si.ycbcr_range = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;

        pyrowave_rate_control rc = {};
        rc.maximum_bitstream_size = budget;
        slp_apply_region_weights();

        // v31: wait here for vrlink's D3D11 side (the Vulkan submit would wait on the same fence anyway) so the
        // latency splits into D3D11 queue time and Vulkan queue time.
        double d3d_ms = 0;
        {
            static HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            if (ev && g_fence->lpVtbl->GetCompletedValue(g_fence) < job.fence_value &&
                SUCCEEDED(g_fence->lpVtbl->SetEventOnCompletion(g_fence, job.fence_value, ev)))
                WaitForSingleObject(ev, 100);
            d3d_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - job.t_submit).count();
        }
        auto t0 = std::chrono::steady_clock::now();
        pyrowave_result r = pyrowave_encoder_encode_gpu_scaled(g_penc, &acquire, &release, &si, &rc);
        auto t1 = std::chrono::steady_clock::now();
        size_t frame_bytes = 0, out_packets = 0;
        std::vector<pyrowave_packet> packets;
        if (r == PYROWAVE_SUCCESS) {
            size_t num_packets = 0;
            r = pyrowave_encoder_compute_num_packets(g_penc, budget, &num_packets);   // waits for the GPU
            if (r == PYROWAVE_SUCCESS && num_packets) {
                packets.resize(num_packets);
                r = pyrowave_encoder_packetize(g_penc, packets.data(), budget, &out_packets, bitstream.data(), bitstream.size());
                for (size_t i = 0; r == PYROWAVE_SUCCESS && i < out_packets; ++i) frame_bytes += packets[i].size;
            }
        }
        auto t2 = std::chrono::steady_clock::now();
        s.busy.store(false);
        if (g_substitute && r == PYROWAVE_SUCCESS && out_packets) {
            // SLPW frame: 32-byte header, packet size table, packets (see frame/decoder/slp_pyro_dec.hpp).
            SubResult res;
            res.index = job.encode_index;
            res.bytes.resize(32 + out_packets * 4);
            uint32_t hdr[8] = {0x57504c53u, 1u | ((g_c444 ? 1u : 0u) << 8) | (32u << 16), g_width, g_height,
                               (uint32_t)job.encode_index, (uint32_t)out_packets, (uint32_t)frame_bytes, 0};
            memcpy(res.bytes.data(), hdr, 32);
            for (size_t i = 0; i < out_packets; ++i) {
                uint32_t sz = (uint32_t)packets[i].size;
                memcpy(res.bytes.data() + 32 + i * 4, &sz, 4);
                res.bytes.insert(res.bytes.end(), bitstream.data() + packets[i].offset,
                                 bitstream.data() + packets[i].offset + packets[i].size);
            }
            push_result(res.index, std::move(res.bytes));
        } else if (g_substitute) {
            push_result(job.encode_index, {});   // failed encode: LockBitstream sends a repeat frame
        }
        if (job.sample) {
            if (r == PYROWAVE_SUCCESS)
                analyze_sample(job, budget_mbps, bitstream.data(), packets, out_packets, frame_bytes);
            g_sample_busy.store(false);
        }

        if (r != PYROWAVE_SUCCESS) {
            block.failed++;
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 5)
                slp_log("[SHADOW] encode of frame #%llu failed: %d", (unsigned long long)job.encode_index, (int)r);
            if (block.failed > 50 && block.frames == 0) { disable("encodes keep failing"); break; }
        } else {
            double lat = std::chrono::duration<double, std::milli>(t2 - job.t_submit).count();
            block.frames++;
            block.bytes += frame_bytes;
            if (frame_bytes > block.bytes_max) block.bytes_max = frame_bytes;
            block.lat_sum_ms += lat;
            block.d3d_sum_ms += d3d_ms;
            if (d3d_ms > block.d3d_max_ms) block.d3d_max_ms = d3d_ms;
            if (lat > block.lat_max_ms) block.lat_max_ms = lat;
            block.call_sum_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
            if (g_substitute) slp_status_frame(lat, d3d_ms, frame_bytes);
            if (done == 0)
                slp_log("[SHADOW] first frame encoded: %zu bytes, latency %.2f ms", frame_bytes, lat);
        }
        ++done;
        if (done % kBlockFrames == 0) {
            log_block(budget_mbps, block);
            block = BlockStats{};
            block.dropped_at_start = g_dropped.load();
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Encoder-thread side
// ---------------------------------------------------------------------------

// D3D11 slot textures (shared NT handles for the Vulkan import), atlas-sized. Releases any previous ones.
bool make_d3d_slots(ID3D11Device *dev, const D3D11_TEXTURE2D_DESC &ad)
{
    constexpr UINT kVulkanD3D11TextureMiscFlags =
        D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    static_assert((kVulkanD3D11TextureMiscFlags & D3D11_RESOURCE_MISC_SHARED) != 0,
                  "NT shared textures also require the shared-resource flag");
    HRESULT hr;
    for (int i = 0; i < kSlots; ++i) {
        safe_release(g_slots[i].tex);
        if (g_slot_handles[i]) { CloseHandle(g_slot_handles[i]); g_slot_handles[i] = nullptr; }
        D3D11_TEXTURE2D_DESC d = ad;
        d.MipLevels = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        d.CPUAccessFlags = 0;
        // VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT imports the NT handle
        // returned by IDXGIResource1::CreateSharedHandle. Keep this paired with
        // SHARED_NTHANDLE on AMD as well; the legacy GetSharedHandle/KMT pairing
        // has different ownership semantics and is not interchangeable.
        d.MiscFlags = kVulkanD3D11TextureMiscFlags;
        hr = dev->lpVtbl->CreateTexture2D(dev, &d, nullptr, &g_slots[i].tex);
        if (FAILED(hr) || !g_slots[i].tex) {
            slp_log("[SHADOW] slot %d CreateTexture2D failed 0x%08lx", i, (unsigned long)hr);
            return false;
        }
        IDXGIResource1 *res1 = nullptr;
        hr = g_slots[i].tex->lpVtbl->QueryInterface(g_slots[i].tex, kIID_IDXGIResource1, (void **)&res1);
        if (SUCCEEDED(hr) && res1) {
            hr = res1->lpVtbl->CreateSharedHandle(res1, nullptr, GENERIC_ALL, nullptr, &g_slot_handles[i]);
            res1->lpVtbl->Release(res1);
        }
        if (FAILED(hr) || !g_slot_handles[i]) {
            slp_log("[SHADOW] slot %d shared handle failed 0x%08lx", i, (unsigned long)hr);
            return false;
        }
        g_slots[i].busy.store(false);
    }
    return true;
}

bool start_on_encoder_thread(ID3D11DeviceContext *ctx, ID3D11Texture2D *atlas)
{
    D3D11_TEXTURE2D_DESC ad;
    atlas->lpVtbl->GetDesc(atlas, &ad);
    if (ad.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || ad.ArraySize != 1 || ad.SampleDesc.Count != 1) {
        slp_log("[SHADOW] atlas has unexpected format %u / arr %u / msaa %u", (unsigned)ad.Format, ad.ArraySize, ad.SampleDesc.Count);
        return false;
    }
    g_width = ad.Width;
    g_height = ad.Height;
    g_ctx = ctx;

    ID3D11Device *dev = nullptr;
    ctx->lpVtbl->GetDevice(ctx, &dev);
    if (!dev) return false;
    bool ok = false;
    ID3D11Device5 *dev5 = nullptr;
    IDXGIDevice *dxgi = nullptr;
    IDXGIAdapter *adapter = nullptr;
    do {
        if (FAILED(dev->lpVtbl->QueryInterface(dev, kIID_ID3D11Device5, (void **)&dev5)) || !dev5) {
            slp_log("[SHADOW] ID3D11Device5 not available"); break;
        }
        if (FAILED(ctx->lpVtbl->QueryInterface(ctx, kIID_ID3D11DeviceContext4, (void **)&g_ctx4)) || !g_ctx4) {
            slp_log("[SHADOW] ID3D11DeviceContext4 not available"); break;
        }
        HRESULT hr = dev5->lpVtbl->CreateFence(dev5, 0, D3D11_FENCE_FLAG_SHARED, kIID_ID3D11Fence, (void **)&g_fence);
        if (FAILED(hr) || !g_fence) { slp_log("[SHADOW] CreateFence failed 0x%08lx", (unsigned long)hr); break; }
        hr = g_fence->lpVtbl->CreateSharedHandle(g_fence, nullptr, GENERIC_ALL, nullptr, &g_fence_handle);
        if (FAILED(hr)) { slp_log("[SHADOW] fence CreateSharedHandle failed 0x%08lx", (unsigned long)hr); break; }

        if (!make_d3d_slots(dev, ad)) break;

        // v11 staging textures for quality / sync sampling (CPU read). Only when sampling is on.
        if (!g_opt_nosample) {
            D3D11_TEXTURE2D_DESC sd = ad;
            sd.MipLevels = 1; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
            if (FAILED(dev->lpVtbl->CreateTexture2D(dev, &sd, nullptr, &g_stage_rgba))) g_stage_rgba = nullptr;
            sd.Format = DXGI_FORMAT_P010;
            if (FAILED(dev->lpVtbl->CreateTexture2D(dev, &sd, nullptr, &g_stage_p010))) g_stage_p010 = nullptr;
            g_p010_w = sd.Width; g_p010_h = sd.Height;
            slp_log("[SHADOW] quality staging textures: rgba %s, p010 %s", g_stage_rgba ? "ok" : "FAILED", g_stage_p010 ? "ok" : "FAILED");
        }

        if (FAILED(dev->lpVtbl->QueryInterface(dev, kIID_IDXGIDevice, (void **)&dxgi)) || !dxgi) break;
        if (FAILED(dxgi->lpVtbl->GetAdapter(dxgi, &adapter)) || !adapter) break;
        {
            // v32: vrlink's D3D11 work (atlas, our prefilter, the copy) waits behind a GPU-bound game. Optionally raise
            // this device's GPU scheduling priority (%TEMP%\slp_d3d_prio = on). Windows may refuse positive values
            // for a non-elevated process; the result is logged either way.
            INT prio = 0;
            dxgi->lpVtbl->GetGPUThreadPriority(dxgi, &prio);
            if (temp_flag_exists("slp_d3d_prio")) {
                HRESULT ph = dxgi->lpVtbl->SetGPUThreadPriority(dxgi, 7);
                INT now = 0;
                dxgi->lpVtbl->GetGPUThreadPriority(dxgi, &now);
                slp_log("[SHADOW] vrlink D3D11 GPU thread priority %d -> requested 7: 0x%08lx, now %d", prio, (unsigned long)ph, now);
            } else {
                slp_log("[SHADOW] vrlink D3D11 GPU thread priority %d (slp_d3d_prio not set)", prio);
            }
        }
        DXGI_ADAPTER_DESC desc = {};
        adapter->lpVtbl->GetDesc(adapter, &desc);
        g_luid = desc.AdapterLuid;
        g_vendor = desc.VendorId;
        g_device_id = desc.DeviceId;
        ok = true;
    } while (false);
    safe_release(adapter);
    safe_release(dxgi);
    safe_release(dev5);
    dev->lpVtbl->Release(dev);
    if (!ok) return false;

    slp_log("[SHADOW] D3D11 side ready: %u slots of %ux%u RGBA16F (shared NT, adapter vendor 0x%04x), shared fence",
            kSlots, g_width, g_height, g_vendor);
    HANDLE th = CreateThread(nullptr, 0, worker_main, nullptr, 0, nullptr);
    if (!th) return false;
    CloseHandle(th);
    return true;
}

} // namespace

static void remember_event(uint64_t index, void *ev)
{
    if (!ev) return;
    std::lock_guard<std::mutex> lk(g_res_mutex);
    g_events[index % 32] = {index, ev};
}

bool slp_shadow_frame(ID3D11DeviceContext *ctx, ID3D11Texture2D *atlas, ID3D11Texture2D *nvenc_input, uint64_t encode_index,
                      void *completion_event)
{
    {
        static auto last = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(now - last).count();
        last = now;
        if (ms > 1.0 && ms < 100.0) g_frame_ms.store(g_frame_ms.load() * 0.95 + ms * 0.05);
    }
    int st = g_state.load();
    if (st == kDisabled || !ctx || !atlas) return false;

    if (st == kOff) {
        // v36: on when settings.json general.enabled = 1 (default); %TEMP%\slp_substitute still forces it on.
        // Off = plain HEVC (no background encoding, unless %TEMP%\slp_shadow_measure asks for the old shadow mode).
        g_substitute = slp_setting("general.enabled", 1.0) != 0 || temp_flag_exists("slp_substitute");
        if (!g_substitute && !temp_flag_exists("slp_shadow_measure")) {
            disable("Pyrowave is turned off in settings.json (general.enabled = 0): plain HEVC until SteamVR restarts");
            return false;
        }
        if (encode_index < (g_substitute ? 1 : kStartAfter)) return false;
        if (temp_flag_exists("slp_shadow_off")) { disable("kill switch %TEMP%\\slp_shadow_off present"); return false; }
        g_state.store(kStarting);
        // v24: sampling only in shadow mode again. In substitution mode it froze the PC (v22/v23: the worker maps
        // staging textures on vrlink's immediate context while vrlink waits for our frames).
        g_opt_nosample = g_substitute || !temp_flag_exists("slp_shadow_sample");
        if (g_substitute) {
            double m = slp_setting("general.multiplier", 1.0);
            if (m >= 0.25 && m <= 4.0) g_multiplier = m;
        }
        g_opt_noencode = temp_flag_exists("slp_shadow_noencode");
        g_opt_noflush  = temp_flag_exists("slp_shadow_noflush");
        slp_log("[SHADOW] switches: nosample=%d noencode=%d noflush=%d", g_opt_nosample, g_opt_noencode, g_opt_noflush);
        if (!start_on_encoder_thread(ctx, atlas)) disable("D3D11 setup failed");
        return false;
    }
    if (st == kResizing) {
        // v25: the stream restarted with another atlas size. Worker released its side (stage 1): make new slots.
        if (g_resize_stage.load() == 1) {
            D3D11_TEXTURE2D_DESC ad;
            atlas->lpVtbl->GetDesc(atlas, &ad);
            ID3D11Device *dev = nullptr;
            ctx->lpVtbl->GetDevice(ctx, &dev);
            bool ok = dev && make_d3d_slots(dev, ad);
            if (dev) dev->lpVtbl->Release(dev);
            if (!ok) { disable("resize: D3D11 slots failed"); g_q_cv.notify_all(); return false; }
            g_width = ad.Width;
            g_height = ad.Height;
            g_resize_stage.store(2);
            g_q_cv.notify_all();
        }
        if (g_substitute) { remember_event(encode_index, completion_event); push_result(encode_index, {}); }
        return g_substitute;
    }
    if (st != kReady) return false;   // worker still initialising Vulkan/Pyrowave
    {
        D3D11_TEXTURE2D_DESC ad;
        atlas->lpVtbl->GetDesc(atlas, &ad);
        if (ad.Width != g_width || ad.Height != g_height) {
            slp_log("[SHADOW] atlas is now %ux%u (was %ux%u): re-creating the encoder", ad.Width, ad.Height, g_width, g_height);
            g_resize_stage.store(0);
            g_state.store(kResizing);
            g_q_cv.notify_all();
            if (g_substitute) { remember_event(encode_index, completion_event); push_result(encode_index, {}); }
            return g_substitute;
        }
    }

    if (encode_index % kBlockFrames == 0 && temp_flag_exists("slp_shadow_off")) {
        disable("kill switch %TEMP%\\slp_shadow_off appeared");
        g_q_cv.notify_all();
        return false;
    }
    if (g_substitute) remember_event(encode_index, completion_event);

    // Pick a free slot; never wait.
    int slot = -1;
    for (int i = 0; i < kSlots; ++i) {
        int k = (g_next_slot + i) % kSlots;
        bool expected = false;
        if (g_slots[k].busy.compare_exchange_strong(expected, true)) { slot = k; break; }
    }
    if (slot < 0) {
        g_dropped.fetch_add(1);
        if (g_substitute) push_result(encode_index, {});   // tell LockBitstream not to wait for it
        return g_substitute;
    }
    g_next_slot = (slot + 1) % kSlots;

    // v23: the stream size must match what we encode. vrlink sizes the atlas when the stream starts; a
    // resolution change during the stream only shrinks NVENC's input, which we cannot follow yet.
    {
        static UINT warned_w = 0, warned_h = 0;
        D3D11_TEXTURE2D_DESC ad, nd;
        atlas->lpVtbl->GetDesc(atlas, &ad);
        if (nvenc_input) nvenc_input->lpVtbl->GetDesc(nvenc_input, &nd); else nd = ad;
        if ((ad.Width != g_width || ad.Height != g_height || nd.Width != g_width || nd.Height != g_height) &&
            (warned_w != nd.Width || warned_h != nd.Height)) {
            warned_w = nd.Width; warned_h = nd.Height;
            slp_log("[SUB] SIZE MISMATCH: encoding %ux%u but the atlas is %ux%u and NVENC's input %ux%u. "
                    "A resolution change during a stream needs a stream restart with Pyrowave (the picture breaks until then).",
                    g_width, g_height, ad.Width, ad.Height, nd.Width, nd.Height);
        }
        if (ad.Width != g_width || ad.Height != g_height) {   // never copy between different sizes
            g_slots[slot].busy.store(false);
            if (g_substitute) push_result(encode_index, {});
            return g_substitute;
        }
    }
    // v25: peripheral prefilter (contrast-aware foveated blur, hidden-area blur); plain copy when off.
    if (!slp_periph_process(ctx, atlas, g_slots[slot].tex))
        ctx->lpVtbl->CopyResource(ctx, (ID3D11Resource *)g_slots[slot].tex, (ID3D11Resource *)atlas);

    // v11: sample this frame?  Copy exactly what Pyrowave will read, plus NVENC's input.
    bool sample = false;
    if (!g_opt_nosample && encode_index % kSampleEvery == kSampleOffset && nvenc_input && g_stage_rgba &&
        !g_sample_busy.exchange(true)) {
        // Follow vrlink's encode size (it can change during a stream).
        D3D11_TEXTURE2D_DESC nd;
        nvenc_input->lpVtbl->GetDesc(nvenc_input, &nd);
        if (!g_stage_p010 || nd.Width != g_p010_w || nd.Height != g_p010_h) {
            if (g_stage_p010) { g_stage_p010->lpVtbl->Release(g_stage_p010); g_stage_p010 = nullptr; }
            ID3D11Device *dv = nullptr;
            ctx->lpVtbl->GetDevice(ctx, &dv);
            D3D11_TEXTURE2D_DESC sd = nd;
            sd.MipLevels = 1; sd.ArraySize = 1; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
            if (dv && SUCCEEDED(dv->lpVtbl->CreateTexture2D(dv, &sd, nullptr, &g_stage_p010))) { g_p010_w = sd.Width; g_p010_h = sd.Height; }
            else g_stage_p010 = nullptr;
            if (dv) dv->lpVtbl->Release(dv);
            slp_log("[SHADOW] NVENC input is %ux%u (atlas %ux%u): p010 staging %s", nd.Width, nd.Height, g_width, g_height,
                    g_stage_p010 ? "recreated" : "FAILED");
        }
        ctx->lpVtbl->CopyResource(ctx, (ID3D11Resource *)g_stage_rgba, (ID3D11Resource *)g_slots[slot].tex);
        if (g_stage_p010) ctx->lpVtbl->CopyResource(ctx, (ID3D11Resource *)g_stage_p010, (ID3D11Resource *)nvenc_input);
        sample = true;
        g_samples.fetch_add(1);
    }
    uint64_t v = ++g_fence_value;
    g_ctx4->lpVtbl->Signal(g_ctx4, g_fence, v);
    if (!g_opt_noflush) ctx->lpVtbl->Flush(ctx);

    {
        std::lock_guard<std::mutex> lk(g_q_mutex);
        g_queue.push_back({slot, v, encode_index, std::chrono::steady_clock::now(), sample});
    }
    g_q_cv.notify_one();
    g_submitted.fetch_add(1);
    return g_substitute;
}

void slp_shadow_counters(uint64_t *submitted, uint64_t *dropped, uint64_t *samples)
{
    *submitted = g_submitted.load();
    *dropped = g_dropped.load();
    *samples = g_samples.load();
}

void push_result(uint64_t index, std::vector<uint8_t> bytes)
{
    void *ev = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_res_mutex);
        g_results.push_back({index, std::move(bytes)});
        while (g_results.size() > 8) g_results.pop_front();
        PendingEvent &pe = g_events[index % 32];
        if (pe.index == index && pe.event) { ev = pe.event; pe = {0, nullptr}; }
    }
    g_res_cv.notify_all();
    if (ev) SetEvent((HANDLE)ev);   // NVENC async mode: vrlink waits on this before LockBitstream
}

bool slp_substitute_active()
{
    return g_substitute && g_state.load() != kDisabled;
}

bool slp_shadow_take_frame(uint64_t encode_index, std::vector<uint8_t> &out, int timeout_ms)
{
    std::unique_lock<std::mutex> lk(g_res_mutex);
    auto find = [&]() -> bool {
        for (auto it = g_results.begin(); it != g_results.end(); ++it)
            if (it->index == encode_index) {
                out.swap(it->bytes);   // empty = dropped / failed
                g_results.erase(g_results.begin(), it + 1);   // older results are never asked for again
                return true;
            }
        return false;
    };
    if (!find()) {
        if (g_state.load() != kReady) return false;
        g_res_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return find(); });
    }
    return !out.empty();
}

// v21: region weights for Pyrowave's rate control (Pyrowave fork: pyrowave_encoder_set_slp_region_weights).
// The "weights" section of settings.json (fovea, wide, under_inset, ...), re-read live.
void slp_apply_region_weights()
{
    using fn_t = void (*)(pyrowave_encoder, float, float, float, float);
    static fn_t fn = nullptr;
    static bool looked = false;
    static float cur[4] = {-1, -1, -1, -1};
    static auto last_check = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    if (!looked) {
        looked = true;
        if (HMODULE h = GetModuleHandleA("libpyrowave-shared-1.dll"))
            fn = reinterpret_cast<fn_t>(reinterpret_cast<void *>(GetProcAddress(h, "pyrowave_encoder_set_slp_region_weights")));
        if (!fn) slp_log("[SUB] region weights: this libpyrowave-shared-1.dll has no pyrowave_encoder_set_slp_region_weights (old build); uniform weighting");
    }
    if (!g_penc) return;
    // v25: per-tile CSF (Nyquist of the inset / wide views / display) from the prefilter settings + locator.
    {
        using csf_t = void (*)(pyrowave_encoder, float, float, float);
        static csf_t csf_fn = nullptr;
        static bool csf_looked = false;
        static float cur_csf[3] = {-1, -1, -1};
        static auto csf_last = std::chrono::steady_clock::now() - std::chrono::seconds(10);
        if (!csf_looked) {
            csf_looked = true;
            if (HMODULE h = GetModuleHandleA("libpyrowave-shared-1.dll"))
                csf_fn = reinterpret_cast<csf_t>(reinterpret_cast<void *>(GetProcAddress(h, "pyrowave_encoder_set_slp_csf")));
            if (!csf_fn) slp_log("[SUB] per-tile CSF: libpyrowave-shared-1.dll is too old (needs v25); desktop CSF");
        }
        auto tnow = std::chrono::steady_clock::now();
        if (g_regions_dirty.load()) cur_csf[0] = -1;
        if (csf_fn && tnow - csf_last > std::chrono::milliseconds(500)) {
            csf_last = tnow;
            float c[3];
            slp_periph_csf(g_width, &c[0], &c[1], &c[2]);
            bool changed = false;
            for (int i = 0; i < 3; ++i)
                if (std::fabs(c[i] - cur_csf[i]) > 0.03f * std::max(1.0f, std::fabs(c[i]))) changed = true;
            if (changed) {
                memcpy(cur_csf, c, sizeof(c));
                csf_fn(g_penc, c[0], c[1], c[2]);
                if (c[1] > 0)
                    slp_log("[SUB] per-tile CSF: inset Nyquist %.1f, wide %.1f, display %.1f cycles/deg", c[0], c[1], c[2]);
                else
                    slp_log("[SUB] per-tile CSF off (Pyrowave desktop model)");
            }
        }
    }
    // v39: inset boxes (per frame), fewer bits under the inset / more along its edge, and the quality floor.
    {
        using box_t = void (*)(pyrowave_encoder, const float *, const float *, float, float, float);
        using floor_t = void (*)(pyrowave_encoder, uint32_t);
        static box_t box_fn = nullptr;
        using ramp_t = void (*)(pyrowave_encoder, float, const float *, const float *);
        static ramp_t ramp_fn = nullptr;   // v45
        static floor_t floor_fn = nullptr;
        static bool v39_looked = false;
        static uint32_t cur_floor = 0xffffffffu;
        static float cur_bp[3] = {-1, -1, -1};
        static float cur_shown = -1;
        if (!v39_looked) {
            v39_looked = true;
            if (HMODULE h = GetModuleHandleA("libpyrowave-shared-1.dll")) {
                box_fn = reinterpret_cast<box_t>(reinterpret_cast<void *>(GetProcAddress(h, "pyrowave_encoder_set_slp_inset_boxes")));
                floor_fn = reinterpret_cast<floor_t>(reinterpret_cast<void *>(GetProcAddress(h, "pyrowave_encoder_set_slp_quality_floor")));
                ramp_fn = reinterpret_cast<ramp_t>(reinterpret_cast<void *>(GetProcAddress(h, "pyrowave_encoder_set_slp_edge_ramp")));
                if (!ramp_fn)
                    slp_log("[SUB] libpyrowave-shared-1.dll is older than v45: the edge ramp leaves the bit weights alone");
            }
            if (!box_fn || !floor_fn)
                slp_log("[SUB] libpyrowave-shared-1.dll is older than v39: no inset-aware weights or quality floor");
        }
        if (g_regions_dirty.load()) { cur_floor = 0xffffffffu; cur_bp[0] = -1; }
        static uint32_t gen = 0;
        static float bp[3] = {1.0f, 0.0f, 0.04f};
        static float shown = 0.7f;
        static uint32_t want_floor = 0;
        uint32_t g = slp_settings_generation();
        if (g != gen) {
            gen = g;
            bp[0] = (float)std::min(1.0, std::max(0.05, slp_setting("weights.under_inset", 0.1)));
            bp[1] = (float)std::min(4.0, std::max(0.0, slp_setting("weights.inset_edge_boost", 0.0)));
            bp[2] = (float)std::min(0.25, std::max(0.005, slp_setting("weights.inset_edge_width", 0.04)));
            // v41: the compositor shows a round part of the inset, smaller than its square: radius = this fraction of
            // the inset half-size (Mia: the full square was "way wider than the center")
            shown = (float)std::min(1.0, std::max(0.2, slp_setting("weights.inset_shown", 0.7)));
            // "Save bits on simple scenes": 0 = off (fill the budget), 1..10 -> rate-control bucket 50..64
            double lv = std::min(10.0, std::max(0.0, slp_setting("general.simple_scene_saving", 0.0)));
            want_floor = lv <= 0.0 ? 0u : (uint32_t)std::lround(50.0 + 1.4 * lv);
        }
        if (box_fn) {
            float box[2][4];
            slp_periph_inset_boxes(box);
            for (int e = 0; e < 2; ++e) {
                box[e][2] = std::min(box[e][2], box[e][3]) * shown;   // disc radius (tile fraction)
                box[e][3] = 0.0f;
            }
            // v45: the edge ramp is centred on the inset too; keep a centre even while the hide box is unknown
            SlpRampInfo ri = slp_periph_ramp();
            float rs[2] = {0, 0}, re[2] = {0, 0};
            if (ramp_fn && ri.ramp > 0.0f) {
                for (int e = 0; e < 2; ++e) {
                    if (box[e][2] <= 0.0f) { box[e][0] = ri.cx[e]; box[e][1] = ri.cy[e]; }
                    slp_ramp_radii(ri, e, &rs[e], &re[e]);
                }
            }
            box_fn(g_penc, box[0], box[1], bp[0], bp[1], bp[2]);
            if (ramp_fn) ramp_fn(g_penc, ri.ramp, rs, re);
            if (memcmp(bp, cur_bp, sizeof(bp)) != 0 || shown != cur_shown) {
                cur_shown = shown;
                memcpy(cur_bp, bp, sizeof(bp));
                slp_log("[SUB] inset-aware weights: under the inset x%.2f, edge band +%.2f (width %.3f), shown part %.2f of the inset",
                        bp[0], bp[1], bp[2], shown);
            }
        }
        if (floor_fn && want_floor != cur_floor) {
            cur_floor = want_floor;
            floor_fn(g_penc, want_floor);
            slp_log("[SUB] simple-scene saving: %s (rate-control floor bucket %u)", want_floor ? "on" : "off", want_floor);
        }
    }
    // v53: 4:2:0 conversion like Steam Link's HEVC path: sharper chroma downsample (8-tap, clamped to each view of
    // the atlas) and luma adjustment (luminance kept on coloured edges). Live from settings.json; 4:4:4 ignores it.
    {
        using y420_t = void (*)(pyrowave_encoder, uint32_t, uint32_t, uint32_t);
        static y420_t y420_fn = nullptr;
        static bool y420_looked = false;
        static uint32_t cur_flags = 0xffffffffu, want_flags = 0;
        static uint32_t gen = 0;
        if (!y420_looked) {
            y420_looked = true;
            if (HMODULE h = GetModuleHandleA("libpyrowave-shared-1.dll"))
                y420_fn = reinterpret_cast<y420_t>(reinterpret_cast<void *>(GetProcAddress(h, "pyrowave_encoder_set_slp_ycbcr420")));
            if (!y420_fn)
                slp_log("[SUB] libpyrowave-shared-1.dll is older than v53: no sharp chroma / luma adjustment");
        }
        uint32_t g = slp_settings_generation();
        if (g != gen) {
            gen = g;
            want_flags = (slp_setting("general.sharp_chroma", 1) != 0 ? 1u : 0u) |
                         (slp_setting("general.luma_adjust", 1) != 0 ? 2u : 0u);
        }
        if (g_regions_dirty.load()) cur_flags = 0xffffffffu;
        if (y420_fn && want_flags != cur_flags) {
            cur_flags = want_flags;
            y420_fn(g_penc, want_flags, g_width, g_height / 4);   // four views stacked vertically
            slp_log("[SUB] 4:2:0 conversion: sharp chroma %s, luma adjustment %s%s", (want_flags & 1u) ? "on" : "off",
                    (want_flags & 2u) ? "on" : "off", g_c444 ? " (not used while full colour is on)" : "");
        }
    }
    if (!fn) return;
    auto now = std::chrono::steady_clock::now();
    if (g_regions_dirty.exchange(false)) { cur[0] = -1; last_check = now - std::chrono::seconds(10); }
    if (now - last_check < std::chrono::seconds(1)) return;
    last_check = now;
    // v36: settings.json section "weights" (fovea, wide); fovea 0 = uniform weighting. v54: the inset middle boost has
    // no control any more (no visible effect); held at the tested value, 2x up to radius 0.5.
    const bool has = true;
    float w[4] = {(float)slp_setting("weights.fovea", 1.0), (float)slp_setting("weights.wide", 1.0), 1.0f, 0.5f};
    if (w[0] > 0) {
        w[0] = std::min(16.0f, w[0]);
        w[1] = std::min(16.0f, std::max(0.01f, w[1]));
        w[2] = std::min(32.0f, std::max(0.0f, w[2]));
        w[3] = std::min(4.0f, std::max(0.05f, w[3]));
    }
    if (memcmp(w, cur, sizeof(w)) == 0) return;
    memcpy(cur, w, sizeof(w));
    fn(g_penc, w[0], w[1], w[2], w[3]);
    if (w[0] > 0)
        slp_log("[SUB] region weights%s: fovea views %.2f (centre up to %.2fx, radius %.2f), wide views %.2f",
                has ? " (settings.json)" : " (defaults)", w[0], 1.0f + w[2], w[3], w[1]);
    else
        slp_log("[SUB] region weights off (uniform, upstream Pyrowave)");
}

void slp_substitute_brake(const char *why)
{
    if (!g_substitute) return;
    auto now = std::chrono::steady_clock::now();
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    // v51: Pyrowave frames are all intra, so a reset / keyframe request means one lost frame, not a broken
    // stream, and vrlink's own target (the 1.0x base) already carries its loss back-off. The brake therefore
    // only trims the part of the multiplier ABOVE 1.0 (general.safety_brake = share trimmed, default 0.5;
    // 0 = off), never below 1.0x, holds 100 ms and recovers linearly over general.brake_recovery_ms (300).
    // At most one brake per 500 ms. (v20: x0.7 down to 0.5x, hold 0.5 s, 5%/100 ms: ~1.2 s to recover.)
    const double strength = std::min(1.0, std::max(0.0, (double)slp_setting("general.safety_brake", 0.0)));
    double before = g_eff_mult.load();
    if (strength <= 0.0 || before <= 1.0) return;
    static std::atomic<int64_t> last_brake_ms{0};
    if (now_ms - last_brake_ms.load() < 500) return;
    last_brake_ms.store(now_ms);
    g_brake_until_ms.store(now_ms + 100);
    double after = 1.0 + (before - 1.0) * (1.0 - strength);
    g_eff_mult.store(after);
    uint64_t n = g_brakes.fetch_add(1) + 1;
    if (n <= 50 || n % 50 == 0)
        slp_log("[SUB] BRAKE #%llu (%s): multiplier %.2f -> %.2f", (unsigned long long)n, why, before, after);
}
