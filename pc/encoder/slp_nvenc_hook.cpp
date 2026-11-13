// Steam Link Pyrowave - NVENC function-table hook for vrserver.exe.
// SPDX-License-Identifier: MIT
//
// nvEncodeAPI64.dll exports only NvEncodeAPICreateInstance (+ GetMaxSupportedVersion).
// All real calls (EncodePicture, LockBitstream, ...) go through a function-pointer
// table that NvEncodeAPICreateInstance fills in.  So we:
//
//   1. Intercept NvEncodeAPICreateInstance, two ways:
//        a. patch the IAT of every non-system module that imports it, and
//        b. patch the IAT of GetProcAddress in those modules so a runtime
//           GetProcAddress("NvEncodeAPICreateInstance") returns our hook.
//      A background thread rescans the module list so late-loaded drivers
//      (driver_vrlink.dll etc.) are covered.
//   2. After the real CreateInstance fills the table, replace selected entries
//      with our hooks.  Before touching the table we verify the slots we are
//      about to patch point inside the same module (sanity check on layout).
//
// With substitution on, EncodePicture hands the atlas to the Pyrowave encoder (slp_shadow.cpp) and LockBitstream
// returns its SLPW frame; NVENC itself is bypassed (v18). Off = stock HEVC. Kill switch: %TEMP%\slp_shadow_off.
//
// Logs go to OutputDebugString (DebugView) and %LOCALAPPDATA%\SteamLinkPyrowave\logs\backend.log.

#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <d3d11.h>
#include <d3d11_1.h>

#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "nvEncodeAPI.h"   // official NVENC 11.1 header (pc/third_party/nvenc)

// Pyrowave C API: Vulkan headers must come first.
#include <vulkan/vulkan.h>
#include "pyrowave.h"

#include "slp_internal.h"
#include "slp_shadow.h"
#include "slp_status.h"
#include "slp_settings.h"
#include "slp_ramp.h"

extern "C" void slp_update_stats(const SlpFrameStats *s);

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

static std::mutex g_log_mutex;
static HANDLE     g_log_file = INVALID_HANDLE_VALUE;

void slp_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 3) n = (int)sizeof(buf) - 3;
    buf[n++] = '\n';
    buf[n] = '\0';

    OutputDebugStringA(buf);

    std::lock_guard<std::mutex> lk(g_log_mutex);
    if (g_log_file == INVALID_HANDLE_VALUE) {
        // v36: %LOCALAPPDATA%\SteamLinkPyrowave\logs\backend.log (was %TEMP%\slp_backend.log)
        std::string lp = slp_data_path("logs\\backend.log");
        {
            g_log_file = CreateFileA(lp.c_str(), FILE_APPEND_DATA,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
    }
    if (g_log_file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_log_file, buf, (DWORD)n, &written, nullptr);
    }
}

// True if %TEMP%\<name> exists.  Used as no-rebuild switches (kill switch, opt-ins).
bool temp_flag_exists(const char *name)
{
    std::string p = slp_temp_path(name);   // v52: ANSI-safe %TEMP% (slp_settings.cpp)
    if (p.empty()) return false;
    if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    // Explorer hides extensions: a "New Text Document" renamed to slp_x is really slp_x.txt.
    return GetFileAttributesA((p + ".txt").c_str()) != INVALID_FILE_ATTRIBUTES;
}

// ---------------------------------------------------------------------------
// Function-table layout (NV_ENCODE_API_FUNCTION_LIST)
//
//   uint32 version; uint32 reserved; then an array of function pointers.
//   Slot indices below are 0-based within that pointer array and come from
//   nvEncodeAPI.h.  We verify they land inside the NVENC module before use.
// ---------------------------------------------------------------------------

namespace slot {
    constexpr size_t OpenEncodeSession   = 0;
    constexpr size_t InitializeEncoder   = 11;
    constexpr size_t EncodePicture       = 16;
    constexpr size_t MapInputResource    = 25;
    constexpr size_t LockBitstream       = 17;
    constexpr size_t UnlockBitstream     = 18;
    constexpr size_t OpenEncodeSessionEx = 29;
    constexpr size_t RegisterResource    = 30;
    constexpr size_t ReconfigureEncoder  = 32;
    constexpr size_t kCheckMax           = 32; // all of 0..32 must be in-module or null
}
constexpr size_t kListHeaderBytes = 8;

using PFN_Generic2 = NVENCSTATUS (NVENCAPI *)(void *, void *);
using PFN_CreateInstance = NVENCSTATUS (NVENCAPI *)(void *);

static std::atomic<FARPROC> g_orig_create{nullptr};
static std::atomic<FARPROC> g_orig_gpa{nullptr};
static std::atomic<FARPROC> g_orig_amf_init{nullptr};
static std::atomic<bool> g_amf_off_logged{false};

static std::atomic<void *> g_orig_encode{nullptr};
static std::atomic<void *> g_orig_lock{nullptr};
static std::atomic<void *> g_orig_init{nullptr};
static std::atomic<void *> g_orig_open_ex{nullptr};
static std::atomic<void *> g_orig_register{nullptr};
static std::atomic<void *> g_orig_map{nullptr};
static std::atomic<void *> g_orig_reconf{nullptr};
static std::atomic<void *> g_orig_unlock{nullptr};
// v18 NVENC bypass (substitution mode): vrlink's EncodePicture / LockBitstream / UnlockBitstream /
// ReconfigureEncoder are answered without touching NVENC (saves ~4 ms idle, ~10 ms under game load).
// %TEMP%\slp_keep_nvenc at stream start keeps NVENC running as in v17.
static std::atomic<int>  g_async{-1};       // NV_ENC_INITIALIZE_PARAMS::enableEncodeAsync
static std::atomic<int>  g_bypass{-1};      // -1 undecided, 0 off, 1 on (decided on the first picture)
static std::atomic<uint64_t> g_init_at_frame{0};   // encode count at the last InitializeEncoder
// Steam Link's current encoder targets, refreshed by (almost) every reconfigure call.
// This is the input for the Pyrowave byte budget ("Steam Link bitrate x multiplier").
std::atomic<uint32_t> g_vrlink_avg_bps{0}, g_vrlink_max_bps{0}, g_vrlink_fps_num{0}, g_vrlink_fps_den{1};
static std::atomic<double> g_vrlink_fast_bps{0.0};
static std::atomic<uint64_t> g_vrlink_dips{0};

static std::atomic<uint64_t> g_encode_calls{0};
static std::atomic<uint64_t> g_lock_calls{0};
static std::atomic<uint64_t> g_lock_bytes{0};

static HMODULE g_self = nullptr;


// ---------------------------------------------------------------------------
// D3D11 probe (observe-only)
//
// Goals: (A) read the registered textures' real DXGI format and share flags
// (Pyrowave can import a D3D11 texture only if it was created MISC_SHARED or
// MISC_SHARED|SHARED_NTHANDLE), and (B) locate vrlink's RGB->P010 conversion by
// logging draws / dispatches / copies / video-processor blits on the immediate
// context of the NVENC device for two frame windows.
//
// COM vtable slots come from the C-interface structs in d3d11.h (CINTERFACE),
// never from hand-counted indices.  The context vtable is shared process-wide,
// so every hook first checks `self` against the NVENC device's context.
// ---------------------------------------------------------------------------

static const GUID kIID_Tex2D =
    {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};
static const GUID kIID_Ctx1 =
    {0xbb2c6faa, 0xb5fb, 0x4082, {0x8e, 0x6b, 0x38, 0x8b, 0x8c, 0xfa, 0x90, 0xe1}};
static const GUID kIID_VideoDevice =
    {0x10ec4d5b, 0x975a, 0x4689, {0xb9, 0xe4, 0xd0, 0xaa, 0xc3, 0x0f, 0xe3, 0x33}};
static const GUID kIID_VideoContext =
    {0x61f21c45, 0x3c0e, 0x4a74, {0x9c, 0xea, 0x67, 0x10, 0x0d, 0x9a, 0xd5, 0xe4}};


static HMODULE module_of(const void *addr);   // defined further down
static bool istarts(const char *s, const char *prefix);

struct IidName { const char *name; GUID iid; };
static const IidName kProbeIids[] = {
    {"ID3D11Texture2D",   {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}}},
    {"ID3D11Resource",    {0xdc8e63f3, 0xd12b, 0x4952, {0xb4, 0x7b, 0x5e, 0x45, 0x02, 0x6a, 0x86, 0x2d}}},
    {"ID3D11Device",      {0xdb6f6ddb, 0xac77, 0x4e88, {0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40}}},
    {"ID3D10Texture2D",   {0x9b7e4c04, 0x342c, 0x4106, {0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0}}},
    {"ID3D10Resource",    {0x9b7e4c01, 0x342c, 0x4106, {0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0}}},
    {"ID3D10Device",      {0x9b7e4c0f, 0x342c, 0x4106, {0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0}}},
    {"IDirect3DSurface9", {0x0cfbaf3a, 0x9ff6, 0x429a, {0x99, 0xb3, 0xa2, 0x79, 0x6a, 0xf8, 0xb8, 0x9b}}},
    {"IDirect3DTexture9", {0x85c31227, 0x3de5, 0x4f00, {0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5}}},
    {"IDirect3DDevice9",  {0xd0223b96, 0xbf7a, 0x43fd, {0x92, 0xbd, 0xa4, 0x3b, 0x0d, 0x82, 0xb9, 0xeb}}},
    {"IDirect3DDevice9Ex",{0xb18b10ce, 0x2649, 0x405a, {0x87, 0x0f, 0x95, 0xf7, 0x77, 0xd4, 0x31, 0x3a}}},
    {"ID3D12Resource",    {0x696442be, 0xa72e, 0x4059, {0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad}}},
    {"ID3D12Device",      {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}}},
    {"IDXGIResource",     {0x035f3ab4, 0x482e, 0x4e50, {0xb4, 0x1f, 0x8a, 0x7f, 0x8b, 0xd8, 0x96, 0x0b}}},
    {"IDXGISurface",      {0xcafcb56c, 0x6ac3, 0x4889, {0xbf, 0x47, 0x9e, 0x23, 0xbb, 0xd2, 0x60, 0xec}}},
    {"IDXGIDevice",       {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}}},
};

// Identify an arbitrary COM pointer: which module implements it and which
// D3D/DXGI interfaces it answers to.  Reads are guarded with VirtualQuery.
static void identify_com(const char *what, void *p)
{
    auto readable = [](const void *a) {
        MEMORY_BASIC_INFORMATION mbi;
        return a && VirtualQuery(a, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
               !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
    };
    if (!readable(p)) { slp_log("[SLP] identify %s: pointer %p not readable", what, p); return; }
    void **vt = *reinterpret_cast<void ***>(p);
    if (!readable(vt)) { slp_log("[SLP] identify %s: bad vtable", what); return; }

    char leaf[MAX_PATH] = "?";
    if (HMODULE m = module_of(vt[0])) {
        char full[MAX_PATH];
        if (GetModuleFileNameA(m, full, MAX_PATH)) {
            const char *l = std::strrchr(full, '\\');
            snprintf(leaf, sizeof(leaf), "%s", l ? l + 1 : full);
        }
    }
    using QI_t = HRESULT (STDMETHODCALLTYPE *)(void *, const GUID &, void **);
    using Rel_t = ULONG (STDMETHODCALLTYPE *)(void *);
    auto qi = reinterpret_cast<QI_t>(vt[0]);

    char hits[512] = "";
    for (const auto &t : kProbeIids) {
        void *out = nullptr;
        if (SUCCEEDED(qi(p, t.iid, &out)) && out) {
            size_t len = std::strlen(hits);
            snprintf(hits + len, sizeof(hits) - len, "%s ", t.name);
            void **ovt = *reinterpret_cast<void ***>(out);
            reinterpret_cast<Rel_t>(ovt[2])(out);
        }
    }
    slp_log("[SLP] identify %s: ptr=%p implementedBy=%s supports: %s", what, p, leaf, hits[0] ? hits : "(none of the probed interfaces)");
}

struct TexEntry { void *tex; void *reg; void *mapped; };
static std::mutex             g_tex_mutex;
static std::vector<TexEntry>  g_texes;

static int tex_index_locked(void *tex)
{
    for (size_t i = 0; i < g_texes.size(); ++i)
        if (g_texes[i].tex == tex) return (int)i;
    return -1;
}

static std::atomic<ID3D11DeviceContext *> g_probe_ctx{nullptr};
static std::atomic<bool> g_probe_installed{false};
static std::atomic<ID3D11DeviceContextVtbl *> g_probe_ctx_vt{nullptr};
static std::atomic<ID3D11DeviceVtbl *>        g_probe_dev_vt{nullptr};
static std::atomic<ID3D11Texture2D *> g_atlas_tex{nullptr};      // R16G16B16A16_FLOAT (RGB atlas)
static std::atomic<ID3D11Texture2D *> g_sharedyuv_tex{nullptr};  // shared P010 conversion target
static std::mutex g_atlas_mutex;

static void fmt_tex(ID3D11Resource *res, char *out, size_t n)
{
    ID3D11Texture2D *t = nullptr;
    if (res && SUCCEEDED(res->lpVtbl->QueryInterface(res, kIID_Tex2D, (void **)&t)) && t) {
        D3D11_TEXTURE2D_DESC d;
        t->lpVtbl->GetDesc(t, &d);
        int idx;
        { std::lock_guard<std::mutex> lk(g_tex_mutex); idx = tex_index_locked(t); }
        char tag[32] = "";
        if (idx >= 0) snprintf(tag, sizeof(tag), " [NVENC-INPUT #%d]", idx);
        snprintf(out, n, "%ux%u dxgi=%u arr=%u mip=%u bind=0x%x misc=0x%x usage=%u%s",
                 d.Width, d.Height, (unsigned)d.Format, d.ArraySize, d.MipLevels,
                 d.BindFlags, d.MiscFlags, (unsigned)d.Usage, tag);
        t->lpVtbl->Release(t);
    } else {
        snprintf(out, n, "(non-tex2d or null)");
    }
}

// --- device creation hook: the only D3D11 vtable patch we keep.
// Context vtables are per-object heap copies that d3d11 rewrites (round 4c), so
// patching them is both useless and racy; ID3D11Device's vtable lives in
// d3d11.dll and is stable.  We only need it to find the RGB atlas texture.
struct DevOrig {
    decltype(ID3D11DeviceVtbl::CreateTexture2D) CreateTexture2D = nullptr;
};
static DevOrig g_co;

static HRESULT STDMETHODCALLTYPE hook_CreateTexture2D(ID3D11Device *d, const D3D11_TEXTURE2D_DESC *desc,
    const D3D11_SUBRESOURCE_DATA *init, ID3D11Texture2D **out)
{
    HRESULT hr = g_co.CreateTexture2D(d, desc, init, out);
    // Identify vrlink's atlas by format and bind flags; its aspect ratio varies by configuration.
    // Our own shadow slots (shared NT, misc 0x800) are not the atlas.
    if (SUCCEEDED(hr) && desc && out && *out && desc->Width >= 256 && desc->Height >= 256) {
        ID3D11Texture2D *keep = nullptr;
        std::atomic<ID3D11Texture2D *> *slot = nullptr;
        // vrlink's atlas is exactly bind SRV|RTV (0x28), no misc flags, one mip. Our own textures (shadow slots
        // misc 0x802, prefilter mip chain misc 0x1, prefilter output bind 0x88) must never be taken for it (v27).
        if (desc->Format == 10 && desc->MiscFlags == 0 && desc->BindFlags == 0x28 && desc->MipLevels <= 1 &&
            desc->Usage == D3D11_USAGE_DEFAULT)
            slot = &g_atlas_tex;
        else if (desc->Format == 104 && (desc->MiscFlags & 0x2)) slot = &g_sharedyuv_tex;
        if (slot) {
            // Newest wins (a stream restart can bring a new atlas); the old one is released.
            keep = *out;
            keep->lpVtbl->AddRef(keep);
            ID3D11Texture2D *old = nullptr;
            if (slot == &g_atlas_tex) {
                std::lock_guard<std::mutex> lk(g_atlas_mutex);
                old = slot->exchange(keep);
            } else {
                old = slot->exchange(keep);
            }
            if (old) old->lpVtbl->Release(old);
            slp_log("[SLP] remembered %s texture %p (%ux%u)", desc->Format == 10 ? "RGB-atlas" : "shared-P010", (void *)keep,
                    desc->Width, desc->Height);
        }
    }
    if (desc && (desc->Format == 103 || desc->Format == 104 || desc->Format == 106 || desc->Format == 10 ||
                 (desc->Width == 1344 && desc->Height >= 1344)))
        slp_log("[SLP] CreateTexture2D %ux%u fmt=%u bind=0x%x misc=0x%x usage=%u arr=%u hr=0x%08lx dev=%p tid=%lu",
                desc->Width, desc->Height, (unsigned)desc->Format, desc->BindFlags, desc->MiscFlags,
                (unsigned)desc->Usage, desc->ArraySize, (unsigned long)hr, (void *)d, GetCurrentThreadId());
    return hr;
}

void slp_nvenc_get_shadow_source(ID3D11DeviceContext **ctx_out, ID3D11Texture2D **atlas_out)
{
    if (!ctx_out || !atlas_out) return;
    *ctx_out = g_probe_ctx.load(std::memory_order_acquire);
    if (*ctx_out) (*ctx_out)->lpVtbl->AddRef(*ctx_out);
    {
        std::lock_guard<std::mutex> lk(g_atlas_mutex);
        *atlas_out = g_atlas_tex.load(std::memory_order_acquire);
        if (*atlas_out) (*atlas_out)->lpVtbl->AddRef(*atlas_out);
    }
}

template <typename F>
static void patch_vt_slot(F *slot_ptr, F hook, F *orig_out)
{
    if (*slot_ptr == hook) return;
    if (!*orig_out) *orig_out = *slot_ptr;
    DWORD old = 0;
    VirtualProtect(slot_ptr, sizeof(F), PAGE_READWRITE, &old);
    *slot_ptr = hook;
    VirtualProtect(slot_ptr, sizeof(F), old, &old);
}

// Called with vrlink's ID3D11Device (an owned reference we keep).
static void probe_install_from_device(ID3D11Device *dev)
{
    if (g_probe_installed.exchange(true)) return;
    ID3D11DeviceContext *ctx = nullptr;
    dev->lpVtbl->GetImmediateContext(dev, &ctx);   // reference kept for the shadow encoder
    if (!ctx) { slp_log("[SLP] probe: no immediate context"); return; }
    g_probe_ctx.store(ctx);
    g_probe_ctx_vt.store(const_cast<ID3D11DeviceContextVtbl *>(ctx->lpVtbl));
    auto dvt = const_cast<ID3D11DeviceVtbl *>(dev->lpVtbl);
    g_probe_dev_vt.store(dvt);
    patch_vt_slot(&dvt->CreateTexture2D, &hook_CreateTexture2D, &g_co.CreateTexture2D);
    slp_log("[SLP] probe: CreateTexture2D hooked on vrlink's device (context is not patched)");
}


// ---------------------------------------------------------------------------
// D3D11CreateDevice interception (IAT of the vrlink driver) + diagnostics
// ---------------------------------------------------------------------------

static std::atomic<FARPROC> g_orig_create_device{nullptr};
static std::mutex           g_dev_mutex;
static std::vector<void *>  g_devices;       // every device vrlink created

static const char *leaf_of_module(const void *addr, char *buf, size_t n)
{
    HMODULE m = module_of(addr);
    wchar_t w[MAX_PATH];
    if (!m || !GetModuleFileNameW(m, w, MAX_PATH)) { snprintf(buf, n, "?"); return buf; }
    char nb[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, w, -1, nb, MAX_PATH, nullptr, nullptr);
    const char *l = std::strrchr(nb, '\\');
    snprintf(buf, n, "%s", l ? l + 1 : nb);
    return buf;
}

typedef HRESULT (WINAPI *PFN_D3D11CreateDevice)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

static HRESULT WINAPI hook_D3D11CreateDevice(IDXGIAdapter *adapter, D3D_DRIVER_TYPE dt, HMODULE sw, UINT flags,
    const D3D_FEATURE_LEVEL *fl, UINT nfl, UINT sdk, ID3D11Device **dev, D3D_FEATURE_LEVEL *outfl,
    ID3D11DeviceContext **ctx)
{
    auto orig = reinterpret_cast<PFN_D3D11CreateDevice>(g_orig_create_device.load());
    if (!orig) return E_FAIL;
    HRESULT hr = orig(adapter, dt, sw, flags, fl, nfl, sdk, dev, outfl, ctx);
    if (SUCCEEDED(hr) && dev && *dev) {
        ID3D11Device *d = *dev;
        ID3D11DeviceContext *ic = nullptr;
        d->lpVtbl->GetImmediateContext(d, &ic);
        char l1[80], l2[80];
        slp_log("[SLP] D3D11CreateDevice: driverType=%d flags=0x%x devPtr=%p devVt=%p(%s) ctxPtr=%p ctxVt=%p(%s) retCtx=%p fl=0x%x tid=%lu",
                (int)dt, flags, (void *)d, (void *)d->lpVtbl, leaf_of_module(d->lpVtbl, l1, sizeof(l1)),
                (void *)ic, ic ? (void *)ic->lpVtbl : nullptr,
                ic ? leaf_of_module(ic->lpVtbl, l2, sizeof(l2)) : "-",
                ctx ? (void *)*ctx : nullptr, outfl ? (unsigned)*outfl : 0u, GetCurrentThreadId());
        { std::lock_guard<std::mutex> lk(g_dev_mutex); g_devices.push_back(d); }
        {
            static const GUID iid_dxgidev = {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
            IDXGIDevice *dd = nullptr;
            if (SUCCEEDED(d->lpVtbl->QueryInterface(d, iid_dxgidev, reinterpret_cast<void **>(&dd))) && dd) {
                IDXGIAdapter *ad = nullptr;
                if (SUCCEEDED(dd->lpVtbl->GetAdapter(dd, &ad)) && ad) {
                    DXGI_ADAPTER_DESC desc = {};
                    ad->lpVtbl->GetDesc(ad, &desc);
                    char nm[128];
                    WideCharToMultiByte(CP_ACP, 0, desc.Description, -1, nm, sizeof(nm), nullptr, nullptr);
                    slp_log("[SLP]   adapter: %s vendor=0x%04x device=0x%04x luid=%08lx:%08lx vram=%zu MB", nm,
                            desc.VendorId, desc.DeviceId, (unsigned long)desc.AdapterLuid.HighPart,
                            (unsigned long)desc.AdapterLuid.LowPart, (size_t)(desc.DedicatedVideoMemory >> 20));
                    ad->lpVtbl->Release(ad);
                }
                dd->lpVtbl->Release(dd);
            }
        }
        if (ic) {
            auto known = g_probe_ctx_vt.load();
            if (known && known != ic->lpVtbl)
                slp_log("[SLP]   NOTE: this context uses a DIFFERENT vtable than the hooked one - its calls are not counted");
            ic->lpVtbl->Release(ic);
        }
        // Hook early, so creation-time resources and the first frames are covered.
        d->lpVtbl->AddRef(d);
        probe_install_from_device(d);
    }
    return hr;
}

static void log_loaded_modules_once()
{
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;
    std::vector<HMODULE> mods(1024);
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods.data(), (DWORD)(mods.size() * sizeof(HMODULE)), &needed)) return;
    mods.resize(std::min<size_t>(needed / sizeof(HMODULE), mods.size()));
    std::string line;
    int n = 0;
    for (HMODULE m : mods) {
        wchar_t w[MAX_PATH];
        if (!GetModuleFileNameW(m, w, MAX_PATH)) continue;
        char nb[MAX_PATH];
        WideCharToMultiByte(CP_ACP, 0, w, -1, nb, MAX_PATH, nullptr, nullptr);
        const char *l = std::strrchr(nb, '\\');
        l = l ? l + 1 : nb;
        // Only names relevant to graphics/compute interop; the full list is noise.
        static const char *keys[] = {"nv", "cuda", "vulkan", "d3d", "dxgi", "amd", "ati", "igd", "ig9", "ig1",
                                     "vrlink", "driver_", "mf", "avrt", "opengl", "ddraw", "dcomp"};
        bool want = false;
        for (auto k : keys) if (istarts(l, k)) { want = true; break; }
        if (!want) continue;
        line += l; line += ' ';
        if (line.size() > 300) { slp_log("[SLP] modules: %s", line.c_str()); line.clear(); }
        ++n;
    }
    if (!line.empty()) slp_log("[SLP] modules: %s", line.c_str());
    slp_log("[SLP] modules: %d graphics-related modules listed (of %zu total)", n, mods.size());
}

// ---------------------------------------------------------------------------
// One-shot frame grab + copy-cost benchmark (runs on the encoder thread)
// ---------------------------------------------------------------------------

struct Mt10Vtbl {   // ID3D10Multithread
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *, const GUID &, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(void *);
    ULONG   (STDMETHODCALLTYPE *Release)(void *);
    void    (STDMETHODCALLTYPE *Enter)(void *);
    void    (STDMETHODCALLTYPE *Leave)(void *);
    BOOL    (STDMETHODCALLTYPE *SetMultithreadProtected)(void *, BOOL);
    BOOL    (STDMETHODCALLTYPE *GetMultithreadProtected)(void *);
};
struct Mt10 { const Mt10Vtbl *lpVtbl; };

static void grab_dump_tex(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *src, const char *name, bool planar_p010)
{
    D3D11_TEXTURE2D_DESC d;
    src->lpVtbl->GetDesc(src, &d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D *st = nullptr;
    HRESULT hr = dev->lpVtbl->CreateTexture2D(dev, &sd, nullptr, &st);
    if (FAILED(hr) || !st) { slp_log("[SLP] grab %s: staging create failed hr=0x%08lx", name, (unsigned long)hr); return; }
    auto t0 = std::chrono::steady_clock::now();
    ctx->lpVtbl->CopyResource(ctx, reinterpret_cast<ID3D11Resource *>(st), reinterpret_cast<ID3D11Resource *>(src));
    ctx->lpVtbl->Flush(ctx);
    D3D11_MAPPED_SUBRESOURCE ms = {};
    hr = ctx->lpVtbl->Map(ctx, reinterpret_cast<ID3D11Resource *>(st), 0, D3D11_MAP_READ, 0, &ms);
    double ms_wait = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (FAILED(hr)) { slp_log("[SLP] grab %s: Map failed hr=0x%08lx", name, (unsigned long)hr); st->lpVtbl->Release(st); return; }
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    char path[MAX_PATH + 64];
    snprintf(path, sizeof(path), "%sslp_grab_%s.raw", tmp, name);
    FILE *f = fopen(path, "wb");
    size_t bytes = 0;
    if (f) {
        auto base = static_cast<const uint8_t *>(ms.pData);
        if (planar_p010) {
            for (UINT y = 0; y < d.Height; ++y)       { fwrite(base + (size_t)y * ms.RowPitch, 1, (size_t)d.Width * 2, f); bytes += (size_t)d.Width * 2; }
            const uint8_t *uv = base + (size_t)d.Height * ms.RowPitch;
            for (UINT y = 0; y < d.Height / 2; ++y)   { fwrite(uv + (size_t)y * ms.RowPitch, 1, (size_t)d.Width * 2, f); bytes += (size_t)d.Width * 2; }
        } else {
            UINT bpp = (d.Format == 10) ? 8 : 4;
            for (UINT y = 0; y < d.Height; ++y)       { fwrite(base + (size_t)y * ms.RowPitch, 1, (size_t)d.Width * bpp, f); bytes += (size_t)d.Width * bpp; }
        }
        fclose(f);
    }
    ctx->lpVtbl->Unmap(ctx, reinterpret_cast<ID3D11Resource *>(st), 0);
    st->lpVtbl->Release(st);
    slp_log("[SLP] grab %s: %ux%u fmt=%u rowPitch=%u copy+map %.1f ms, wrote %zu bytes -> %s", name, d.Width, d.Height,
            (unsigned)d.Format, ms.RowPitch, ms_wait, bytes, f ? path : "(open failed)");
}

static void bench_copy(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *src, const char *name, int iters)
{
    D3D11_TEXTURE2D_DESC d;
    src->lpVtbl->GetDesc(src, &d);
    D3D11_TEXTURE2D_DESC cd = d;
    cd.BindFlags = 0; cd.MiscFlags = 0; cd.CPUAccessFlags = 0; cd.Usage = D3D11_USAGE_DEFAULT;
    ID3D11Texture2D *dst = nullptr;
    HRESULT hr = dev->lpVtbl->CreateTexture2D(dev, &cd, nullptr, &dst);
    if (FAILED(hr) || !dst) { slp_log("[SLP] bench %s: dst create failed hr=0x%08lx", name, (unsigned long)hr); return; }
    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
    ID3D11Query *q = nullptr;
    if (FAILED(dev->lpVtbl->CreateQuery(dev, &qd, &q)) || !q) { dst->lpVtbl->Release(dst); return; }
    auto sync = [&]() {
        ctx->lpVtbl->End(ctx, reinterpret_cast<ID3D11Asynchronous *>(q));
        ctx->lpVtbl->Flush(ctx);
        BOOL done = FALSE;
        while (ctx->lpVtbl->GetData(ctx, reinterpret_cast<ID3D11Asynchronous *>(q), &done, sizeof(done), 0) != S_OK) Sleep(0);
    };
    sync();
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i)
        ctx->lpVtbl->CopyResource(ctx, reinterpret_cast<ID3D11Resource *>(dst), reinterpret_cast<ID3D11Resource *>(src));
    sync();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / iters;
    slp_log("[SLP] bench %s: CopyResource DEFAULT->DEFAULT avg %.3f ms over %d (under live stream load; budget 6.94 ms/frame)", name, ms, iters);
    q->lpVtbl->Release(q);
    dst->lpVtbl->Release(dst);
}

static void grab_once(void *ring_tex)
{
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;
    ID3D11DeviceContext *ctx = g_probe_ctx.load();
    if (!ctx) { slp_log("[SLP] grab: no context"); return; }
    ID3D11Device *dev = nullptr;
    ctx->lpVtbl->GetDevice(ctx, &dev);
    if (!dev) return;

    // Serialise with vrlink's own use of the immediate context for the duration.
    static const GUID iid_mt = {0x9b7e4e00, 0x342c, 0x4106, {0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0}};
    Mt10 *mt = nullptr;
    BOOL was = FALSE;
    if (SUCCEEDED(ctx->lpVtbl->QueryInterface(ctx, iid_mt, reinterpret_cast<void **>(&mt))) && mt) {
        was = mt->lpVtbl->SetMultithreadProtected(mt, TRUE);
        slp_log("[SLP] grab: multithread protection was %d, now on for the grab", (int)was);
    } else {
        slp_log("[SLP] grab: ID3D10Multithread not available");
    }

    slp_log("[SLP] grab: starting on tid=%lu (expect one hitch in the stream)", GetCurrentThreadId());
    if (auto t = g_atlas_tex.load())     { grab_dump_tex(dev, ctx, t, "atlas_rgba16f", false); bench_copy(dev, ctx, t, "atlas_rgba16f", 30); }
    if (auto t = g_sharedyuv_tex.load()) { grab_dump_tex(dev, ctx, t, "sharedyuv_p010", true);  bench_copy(dev, ctx, t, "sharedyuv_p010", 30); }
    if (ring_tex) grab_dump_tex(dev, ctx, static_cast<ID3D11Texture2D *>(ring_tex), "ring_p010", true);

    if (mt) {
        if (!was) mt->lpVtbl->SetMultithreadProtected(mt, FALSE);
        mt->lpVtbl->Release(mt);
    }
    dev->lpVtbl->Release(dev);
    slp_log("[SLP] grab: done");
}

// ---------------------------------------------------------------------------
// Hooked NVENC entry points (observe + pass through)
// ---------------------------------------------------------------------------

static NVENCSTATUS NVENCAPI hook_OpenEncodeSessionEx(void *params, void *encoder_out)
{
    auto orig = reinterpret_cast<PFN_Generic2>(g_orig_open_ex.load());
    if (!orig) return NV_ENC_ERR_INVALID_CALL;
    void *device = nullptr;
    if (params) {
        // NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS: u32 version; u32 deviceType; void* device;
        // deviceType: 0 = DirectX, 1 = CUDA, 2 = OpenGL.
        auto p = static_cast<const uint8_t *>(params);
        uint32_t version, dev_type;
        std::memcpy(&version, p, 4);
        std::memcpy(&dev_type, p + 4, 4);
        std::memcpy(&device, p + 8, 8);
        slp_log("[SLP] NvEncOpenEncodeSessionEx: version=0x%08x deviceType=%u (0=DX,1=CUDA,2=GL)",
                version, dev_type);
        if (dev_type == 0) {
            identify_com("NVENC device", device);
            std::lock_guard<std::mutex> lk(g_dev_mutex);
            bool found = false;
            for (void *d : g_devices) if (d == device) found = true;
            slp_log("[SLP] NVENC device %p %s among %zu devices created via D3D11CreateDevice", device,
                    found ? "IS" : "is NOT", g_devices.size());
        }
    }
    NVENCSTATUS r = orig(params, encoder_out);
    if (r == NV_ENC_SUCCESS && device) {
        // If it is a D3D11 device, hook its immediate context (keeps one reference).
        using QI_t = HRESULT (STDMETHODCALLTYPE *)(void *, const GUID &, void **);
        static const GUID iid_dev11 =
            {0xdb6f6ddb, 0xac77, 0x4e88, {0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40}};
        void **vt = *reinterpret_cast<void ***>(device);
        void *d11 = nullptr;
        if (SUCCEEDED(reinterpret_cast<QI_t>(vt[0])(device, iid_dev11, &d11)) && d11)
            probe_install_from_device(static_cast<ID3D11Device *>(d11));
        else
            slp_log("[SLP] probe: NVENC device is not an ID3D11Device - context tracing skipped");
    }
    return r;
}


// v54: what vrlink asks NVENC for, beyond the bitrate (read-only): adaptive quantisation, QP maps, tuning, HEVC
// tools. Logged once per distinct configuration; tells whether Steam Link shapes the periphery inside the encoder.
static void log_nvenc_config(const NV_ENC_INITIALIZE_PARAMS &ip)
{
    if (!ip.encodeConfig || ip.encodeConfig->version != NV_ENC_CONFIG_VER) return;
    const NV_ENC_CONFIG &c = *ip.encodeConfig;
    const NV_ENC_RC_PARAMS &rc = c.rcParams;
    const NV_ENC_CONFIG_HEVC &h = c.encodeCodecConfig.hevcConfig;
    char line[512];
    snprintf(line, sizeof(line),
             "preset %08lx tuning %u profile %08lx | AQ %u (strength %u) temporal AQ %u, QP map mode %u, lookahead %u "
             "(depth %u), multipass %u, min QP %u (%u/%u/%u) max QP %u (%u/%u/%u), init QP %u, zero-latency %u | HEVC: "
             "chroma %u, bit depth %u, CU %u-%u, intra refresh %u (period %u cnt %u), LTR %u, slice mode %u/%u, refs %u",
             (unsigned long)ip.presetGUID.Data1, (unsigned)ip.tuningInfo, (unsigned long)c.profileGUID.Data1,
             rc.enableAQ, rc.aqStrength, rc.enableTemporalAQ, (unsigned)rc.qpMapMode, rc.enableLookahead,
             rc.lookaheadDepth, (unsigned)rc.multiPass, rc.enableMinQP, rc.minQP.qpInterP, rc.minQP.qpInterB,
             rc.minQP.qpIntra, rc.enableMaxQP, rc.maxQP.qpInterP, rc.maxQP.qpInterB, rc.maxQP.qpIntra,
             rc.enableInitialRCQP, rc.zeroReorderDelay, h.chromaFormatIDC, 8 + h.pixelBitDepthMinus8,
             (unsigned)h.minCUSize, (unsigned)h.maxCUSize, h.enableIntraRefresh, h.intraRefreshPeriod,
             h.intraRefreshCnt, h.enableLTR, h.sliceMode, h.sliceModeData, h.maxNumRefFramesInDPB);
    static std::string last;
    if (last == line) return;
    last = line;
    slp_log("[SLP]   NVENC config: %s", line);
}

// v54: per-picture QP delta / emphasis map from vrlink, summarised (min / max / mean, and a coarse grid of means
// over the 4-view atlas so a foveation pattern shows). Logged at a few frames and when it changes.
static void log_qp_map(const NV_ENC_PIC_PARAMS &pp, uint64_t n)
{
    if (pp.version != NV_ENC_PIC_PARAMS_VER) return;
    static int state = -1;   // -1 unknown, 0 no map, 1 map
    const int8_t *m = pp.qpDeltaMap;
    const uint32_t sz = pp.qpDeltaMapSize;
    if (!m || !sz || !pp.inputWidth || !pp.inputHeight) {
        if (state != 0) slp_log("[SLP]   picture #%llu: no QP map", (unsigned long long)n);
        state = 0;
        return;
    }
    if (state == 1 && n % 7200 != 0) return;
    state = 1;
    uint32_t ctb = 0, cw = 0, chh = 0;
    for (uint32_t c : {16u, 32u, 64u}) {
        uint32_t w = (pp.inputWidth + c - 1) / c, hh = (pp.inputHeight + c - 1) / c;
        if (w * hh == sz) { ctb = c; cw = w; chh = hh; }
    }
    int mn = 127, mx = -128;
    double sum = 0;
    for (uint32_t i = 0; i < sz; ++i) { mn = std::min(mn, (int)m[i]); mx = std::max(mx, (int)m[i]); sum += m[i]; }
    slp_log("[SLP]   picture #%llu: QP map %u bytes (block %u px, %ux%u), values %d..%d, mean %.2f",
            (unsigned long long)n, sz, ctb, cw, chh, mn, mx, sum / sz);
    if (!ctb) return;
    const int GX = 8, GY = 16;   // 8 columns x 4 rows per view
    for (int gy = 0; gy < GY; ++gy) {
        char row[128];
        int o = 0;
        for (int gx = 0; gx < GX; ++gx) {
            uint32_t x0 = cw * gx / GX, x1 = std::max(x0 + 1, cw * (gx + 1) / GX);
            uint32_t y0 = chh * gy / GY, y1 = std::max(y0 + 1, chh * (gy + 1) / GY);
            double s2 = 0;
            int k = 0;
            for (uint32_t y = y0; y < y1 && y < chh; ++y)
                for (uint32_t x = x0; x < x1 && x < cw; ++x) { s2 += m[y * cw + x]; ++k; }
            o += snprintf(row + o, sizeof(row) - o, " %5.1f", k ? s2 / k : 0.0);
        }
        slp_log("[SLP]     view %d row %d:%s", gy / 4, gy % 4, row);
    }
}

static NVENCSTATUS NVENCAPI hook_InitializeEncoder(void *encoder, void *params)
{
    auto orig = reinterpret_cast<PFN_Generic2>(g_orig_init.load());
    if (!orig) return NV_ENC_ERR_INVALID_CALL;
    if (params) {
        auto ip = static_cast<const NV_ENC_INITIALIZE_PARAMS *>(params);
        if (ip->version == NV_ENC_INITIALIZE_PARAMS_VER) g_async.store(ip->enableEncodeAsync ? 1 : 0);
        g_bypass.store(-1);
        g_init_at_frame.store(g_encode_calls.load());
        slp_log("[SLP] NvEncInitializeEncoder: ver=0x%08x encode %ux%u max %ux%u fps %u/%u encodeGUID.Data1=0x%08lx async=%d",
                ip->version, ip->encodeWidth, ip->encodeHeight, ip->maxEncodeWidth, ip->maxEncodeHeight,
                ip->frameRateNum, ip->frameRateDen, (unsigned long)ip->encodeGUID.Data1, g_async.load());
        if (ip->version == NV_ENC_INITIALIZE_PARAMS_VER && ip->encodeConfig) {
            const NV_ENC_RC_PARAMS &rc = ip->encodeConfig->rcParams;
            slp_log("[SLP]   rate control: mode=0x%x avg %u bps, max %u bps, vbv %u bits, gop %u",
                    (unsigned)rc.rateControlMode, rc.averageBitRate, rc.maxBitRate, rc.vbvBufferSize,
                    ip->encodeConfig->gopLength);
            log_nvenc_config(*ip);
        }
    }
    return orig(encoder, params);
}

static NVENCSTATUS NVENCAPI hook_RegisterResource(void *encoder, void *params)
{
    auto orig = reinterpret_cast<PFN_Generic2>(g_orig_register.load());
    if (!orig) return NV_ENC_ERR_INVALID_CALL;
    uint32_t type = 0xffffffffu;
    void *tex = nullptr;
    if (params) {
        // NV_ENC_REGISTER_RESOURCE: u32 version; u32 resourceType; u32 width; u32 height;
        // u32 pitch; u32 subResourceIndex; void* resourceToRegister; void* registered;
        // u32 bufferFormat; u32 bufferUsage;
        // resourceType: 0=DX texture,1=CUDA devptr,2=CUDA array,3=OpenGL tex.
        auto p = static_cast<const uint8_t *>(params);
        uint32_t w, h, pitch, fmt;
        std::memcpy(&type,  p + 4,  4);
        std::memcpy(&w,     p + 8,  4);
        std::memcpy(&h,     p + 12, 4);
        std::memcpy(&pitch, p + 16, 4);
        std::memcpy(&tex,   p + 24, 8);
        std::memcpy(&fmt,   p + 40, 4);
        slp_log("[SLP] NvEncRegisterResource: type=%u %ux%u pitch=%u bufferFormat=0x%08x",
                type, w, h, pitch, fmt);
    }
    NVENCSTATUS r = orig(encoder, params);
    if (r == NV_ENC_SUCCESS && params && type == 0 && tex) {
        void *reg = nullptr;
        std::memcpy(&reg, static_cast<const uint8_t *>(params) + 32, 8);
        int idx;
        {
            std::lock_guard<std::mutex> lk(g_tex_mutex);
            idx = tex_index_locked(tex);
            if (idx < 0 && g_texes.size() < 32) {
                g_texes.push_back({tex, reg, nullptr});
                idx = (int)g_texes.size() - 1;
            } else if (idx >= 0) {
                g_texes[idx].reg = reg;
            }
        }
        char d[300];
        fmt_tex(static_cast<ID3D11Resource *>(tex), d, sizeof(d));
        slp_log("[SLP]   registered texture #%d: %s", idx, d);
        static std::atomic<bool> ident_done{false};
        if (!ident_done.exchange(true)) identify_com("registered resource #0", tex);
    }
    return r;
}

static NVENCSTATUS NVENCAPI hook_MapInputResource(void *encoder, void *params)
{
    auto orig = reinterpret_cast<PFN_Generic2>(g_orig_map.load());
    if (!orig) return NV_ENC_ERR_INVALID_CALL;
    NVENCSTATUS r = orig(encoder, params);
    if (r == NV_ENC_SUCCESS && params) {
        // NV_ENC_MAP_INPUT_RESOURCE: u32 version; u32 subResourceIndex; void* inputResource;
        // void* registeredResource; void* mappedResource; u32 mappedBufferFmt;
        auto p = static_cast<const uint8_t *>(params);
        void *reg = nullptr, *mapped = nullptr;
        std::memcpy(&reg,    p + 16, 8);
        std::memcpy(&mapped, p + 24, 8);
        std::lock_guard<std::mutex> lk(g_tex_mutex);
        for (size_t i = 0; i < g_texes.size(); ++i) {
            if (g_texes[i].reg == reg) {
                g_texes[i].mapped = mapped;
                static std::atomic<int> logged{0};
                if (logged.fetch_add(1) < 8)
                    slp_log("[SLP] NvEncMapInputResource: registered #%zu -> mapped handle set", i);
                break;
            }
        }
    }
    return r;
}

// ---------------------------------------------------------------------------
// v13: per-second NVENC timing, to find what the shadow encoder does to vrlink's
// "Enc" time once per second.  enc = EncodePicture call -> vrlink's LockBitstream
// for that output buffer (vrlink locks right after the completion event fires, so
// this is NVENC completion + wake-up).  Also times nvEncReconfigureEncoder, which
// vrlink calls for bitrate updates.
// ---------------------------------------------------------------------------
static double now_ms()
{
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
struct InFlight { const void *out; double t; uint64_t n; bool bypassed; uint32_t frame_idx; uint64_t ts; uint64_t dur; };
static std::mutex g_tm_mutex;
static std::mutex g_bypass_mutex;
static std::set<const void *> g_bypassed_locked;   // output buffers locked via the bypass (Unlock must not reach NVENC)
static InFlight   g_inflight[32];
static int        g_inflight_next = 0;
struct SecStats {
    double   win_start = 0, t0 = 0;
    uint32_t n = 0;
    double   sum = 0, max = 0, max_at = 0;
    uint64_t max_frame = 0;
    double   call_max = 0;           // CPU time inside nvEncEncodePicture
    uint32_t reconf = 0;
    double   reconf_ms = 0, reconf_at = 0;
    int      windows = 0;
    uint64_t sh_sub = 0, sh_drop = 0, sh_samp = 0;
};
static SecStats g_sec;

static void sec_roll_locked(double t)
{
    if (g_sec.win_start == 0) { g_sec.win_start = g_sec.t0 = t; return; }
    if (t - g_sec.win_start < 1000.0) return;
    uint64_t sub = 0, drop = 0, samp = 0;
    slp_shadow_counters(&sub, &drop, &samp);
    if (g_sec.windows < 240)
        slp_log("[NVT] t=%6.1fs frames %u enc avg %.2f max %.2f ms (@+%.0f ms, #%llu) encPicCall max %.2f ms; "
                "reconfigure %u (%.2f ms @+%.0f ms, vrlink avg %.1f Mbps); shadow submits %llu drops %llu samples %llu",
                (g_sec.win_start - g_sec.t0) / 1000.0, g_sec.n, g_sec.n ? g_sec.sum / g_sec.n : 0.0, g_sec.max,
                g_sec.max_at, (unsigned long long)g_sec.max_frame, g_sec.call_max, g_sec.reconf, g_sec.reconf_ms,
                g_sec.reconf_at, g_vrlink_avg_bps.load() / 1e6, (unsigned long long)(sub - g_sec.sh_sub), (unsigned long long)(drop - g_sec.sh_drop),
                (unsigned long long)(samp - g_sec.sh_samp));
    g_sec.windows++;
    slp_status_set("vrlink_target_mbps", g_vrlink_avg_bps.load() / 1e6);
    g_sec.win_start += 1000.0 * std::floor((t - g_sec.win_start) / 1000.0);
    g_sec.n = 0; g_sec.sum = g_sec.max = g_sec.max_at = g_sec.call_max = 0; g_sec.max_frame = 0;
    g_sec.reconf = 0; g_sec.reconf_ms = g_sec.reconf_at = 0;
    g_sec.sh_sub = sub; g_sec.sh_drop = drop; g_sec.sh_samp = samp;
}


static void note_reconfigure(const void *params)
{
    auto rp = static_cast<const NV_ENC_RECONFIGURE_PARAMS *>(params);
    if (!rp || rp->version != NV_ENC_RECONFIGURE_PARAMS_VER) {
        static std::atomic<int> warned{0};
        if (rp && warned.fetch_add(1) < 3) slp_log("[SLP] reconfigure: unexpected struct version 0x%08x", rp->version);
        return;
    }
    const NV_ENC_INITIALIZE_PARAMS &ip = rp->reInitEncodeParams;
    if (!ip.encodeConfig) return;
    const NV_ENC_RC_PARAMS &rc = ip.encodeConfig->rcParams;
    uint32_t prev_avg = g_vrlink_avg_bps.exchange(rc.averageBitRate);
    // v20: vrlink reconfigures every frame and its target jitters frame to frame (v19 braked on those
    // single-frame dips ~30 times a minute). Track a fast (~0.2 s) and a slow (~2 s) average of the target;
    // only a sustained cut (fast < 75% of slow), an encoder reset or a keyframe request is a brake signal.
    {
        static double fast = 0, slow = 0;
        double x = rc.averageBitRate;
        if (slow <= 0 || rp->resetEncoder) fast = slow = x;
        fast += (x - fast) / 29.0;
        slow += (x - slow) / 288.0;
        g_vrlink_fast_bps.store(fast);
        if (prev_avg && x < prev_avg * 0.85) g_vrlink_dips.fetch_add(1);
        if (g_encode_calls.load() - g_init_at_frame.load() > 300) {
            if (rp->resetEncoder) slp_substitute_brake("vrlink reset the encoder");
            else if (rp->forceIDR) slp_substitute_brake("vrlink requested a keyframe");
            else if (fast < 0.75 * slow) slp_substitute_brake("vrlink cut its target for 0.2 s+");
        }
    }
    uint32_t prev_max = g_vrlink_max_bps.exchange(rc.maxBitRate);
    uint32_t prev_fn  = g_vrlink_fps_num.exchange(ip.frameRateNum);
    g_vrlink_fps_den.store(ip.frameRateDen ? ip.frameRateDen : 1);
    static std::atomic<int> logged{0};
    if ((prev_avg != rc.averageBitRate || prev_max != rc.maxBitRate || prev_fn != ip.frameRateNum) &&
        logged.fetch_add(1) < 60)
        slp_log("[SLP] reconfigure: %ux%u fps %u/%u, rc mode 0x%x avg %.1f Mbps max %.1f Mbps vbv %u bits, resetEncoder=%u forceIDR=%u",
                ip.encodeWidth, ip.encodeHeight, ip.frameRateNum, ip.frameRateDen, (unsigned)rc.rateControlMode,
                rc.averageBitRate / 1e6, rc.maxBitRate / 1e6, rc.vbvBufferSize, rp->resetEncoder, rp->forceIDR);
    log_nvenc_config(ip);   // v54: logged again only if the non-bitrate settings (QP map mode, AQ, ...) change
}

static NVENCSTATUS NVENCAPI hook_ReconfigureEncoder(void *encoder, void *params)
{
    auto orig = reinterpret_cast<PFN_Generic2>(g_orig_reconf.load());
    if (!orig) return NV_ENC_ERR_INVALID_CALL;
    note_reconfigure(params);
    double t0 = now_ms();
    // Bypass: only the bitrate is needed (read above); NVENC itself is idle.
    NVENCSTATUS r = g_bypass.load() == 1 ? NV_ENC_SUCCESS : orig(encoder, params);
    double t1 = now_ms();
    std::lock_guard<std::mutex> lk(g_tm_mutex);
    sec_roll_locked(t0);
    g_sec.reconf++;
    if (t1 - t0 >= g_sec.reconf_ms) { g_sec.reconf_ms = t1 - t0; g_sec.reconf_at = t0 - g_sec.win_start; }
    return r;
}

static NVENCSTATUS NVENCAPI hook_EncodePicture(void *encoder, void *params)
{
    auto orig = reinterpret_cast<PFN_Generic2>(g_orig_encode.load());
    if (!orig) return NV_ENC_ERR_INVALID_CALL;

    uint64_t n = g_encode_calls.fetch_add(1) + 1;

    auto pp_in = static_cast<const NV_ENC_PIC_PARAMS *>(params);
    bool real_picture = pp_in && pp_in->inputBuffer && pp_in->inputWidth && pp_in->inputHeight;

    // v10 shadow encode: copy the RGB atlas and encode it with Pyrowave 4:4:4 on a
    // worker thread.  NVENC still gets its frame; nothing is substituted.
    bool will_result = false;
    void *ev = (real_picture && g_async.load() == 1) ? pp_in->completionEvent : nullptr;
    if (real_picture) {
        ID3D11DeviceContext *shadow_ctx = nullptr;
        ID3D11Texture2D *atlas = nullptr;
        slp_nvenc_get_shadow_source(&shadow_ctx, &atlas);
        void *ring = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_tex_mutex);
            for (size_t i = 0; i < g_texes.size(); ++i)
                if (g_texes[i].mapped && g_texes[i].mapped == pp_in->inputBuffer) { ring = g_texes[i].tex; break; }
        }
        will_result = slp_shadow_frame(shadow_ctx, atlas, static_cast<ID3D11Texture2D *>(ring), n, ev);
        if (atlas) atlas->lpVtbl->Release(atlas);
        if (shadow_ctx) shadow_ctx->lpVtbl->Release(shadow_ctx);
        if (g_bypass.load() == -1 && slp_substitute_active()) {
            int b = temp_flag_exists("slp_keep_nvenc") ? 0 : 1;
            g_bypass.store(b);
            slp_log("[SLP] NVENC %s (async mode %d)", b ? "BYPASSED: Pyrowave answers EncodePicture/LockBitstream directly"
                                                       : "kept running (%TEMP%\\slp_keep_nvenc)", g_async.load());
        }
    }

    // One-shot raw frame grab, opt-in only (create %TEMP%\slp_grab_enable).
    if (n == 600 && real_picture && temp_flag_exists("slp_grab_enable")) {
        auto pp0 = static_cast<const NV_ENC_PIC_PARAMS *>(params);
        void *ring = nullptr;
        { std::lock_guard<std::mutex> lk(g_tex_mutex);
          for (size_t i = 0; i < g_texes.size(); ++i)
              if (g_texes[i].mapped && g_texes[i].mapped == pp0->inputBuffer) { ring = g_texes[i].tex; break; } }
        grab_once(ring);
    }
    if (n == 30) log_loaded_modules_once();

    if (n % 720 == 0) {
        static auto last_t = std::chrono::steady_clock::now();
        static uint64_t last_n = 0, last_b = 0;
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last_t).count();
        uint64_t b = g_lock_bytes.load();
        if (dt > 0.01 && last_n)
            slp_log("[SLP] rate: %.1f fps, %.1f Mbps avg, %.0f B/frame avg (last %llu frames)",
                    (n - last_n) / dt, (b - last_b) * 8.0 / dt / 1e6,
                    (double)(b - last_b) / (double)(n - last_n), (unsigned long long)(n - last_n));
        last_t = now; last_n = n; last_b = b;
    }
    if (real_picture && (n == 5 || n == 600 || n % 7200 == 0)) log_qp_map(*pp_in, n);
    if (params && (n <= 5 || n % 720 == 0)) {
        // Only leading, stable fields of NV_ENC_PIC_PARAMS are read.
        auto pp = static_cast<const NV_ENC_PIC_PARAMS *>(params);
        int tex_idx = -1;
        {
            std::lock_guard<std::mutex> lk(g_tex_mutex);
            for (size_t i = 0; i < g_texes.size(); ++i)
                if (g_texes[i].mapped && g_texes[i].mapped == pp->inputBuffer) { tex_idx = (int)i; break; }
        }
        slp_log("[SLP] NvEncEncodePicture #%llu: %ux%u pitch=%u fmt=0x%08x frameIdx=%u inputTexture=#%d",
                (unsigned long long)n, pp->inputWidth, pp->inputHeight,
                pp->inputPitch, pp->bufferFmt, pp->frameIdx, tex_idx);
    }
    double t0 = now_ms();
    const bool bypass = real_picture && g_bypass.load() == 1;
    NVENCSTATUS r = NV_ENC_SUCCESS;
    if (bypass) {
        // No result will arrive for this picture (shadow not ready): let vrlink proceed to a repeat frame.
        if (ev && !will_result) SetEvent((HANDLE)ev);
    } else {
        r = orig(encoder, params);
    }
    double t1 = now_ms();
    if (real_picture) {
        std::lock_guard<std::mutex> lk(g_tm_mutex);
        g_inflight[g_inflight_next] = {pp_in->outputBitstream, t0, n, bypass, pp_in->frameIdx, pp_in->inputTimeStamp,
                                       pp_in->inputDuration};
        g_inflight_next = (g_inflight_next + 1) % 32;
        if (t1 - t0 > g_sec.call_max) g_sec.call_max = t1 - t0;
    }
    return r;
}

// v20: the ~0.2 s average of vrlink's target (single-frame jitter removed).
double slp_vrlink_avg_bps() { double f = g_vrlink_fast_bps.load(); return f > 0 ? f : (double)g_vrlink_avg_bps.load(); }
void slp_vrlink_amf_target_bps(uint64_t target_bps)
{
    if (!target_bps || target_bps > UINT32_MAX) return;
    const uint32_t target = static_cast<uint32_t>(target_bps);
    const uint32_t previous = g_vrlink_avg_bps.exchange(target);
    g_vrlink_fast_bps.store(static_cast<double>(target));
    if (previous != target) {
        slp_log("[SLP] AMF target bitrate %.1f Mbps", target / 1e6);
        slp_status_set("vrlink_target_mbps", target / 1e6);
    }
}
uint64_t slp_vrlink_dips_take() { return g_vrlink_dips.exchange(0); }

// v17 substitution: per output-bitstream buffer we keep the Pyrowave bytes handed to vrlink
// alive until the next lock of that buffer (vrlink copies them out before unlocking).
struct SubBuf { const void *out = nullptr; std::vector<uint8_t> bytes; };
static SubBuf g_subbuf[32];
static std::mutex g_sub_mutex;
static std::atomic<uint64_t> g_sub_frames{0}, g_sub_repeats{0}, g_sub_bytes{0};

static std::vector<uint8_t> &subbuf_for(const void *out)
{
    for (auto &b : g_subbuf) if (b.out == out) return b.bytes;
    for (auto &b : g_subbuf) if (!b.out) { b.out = out; return b.bytes; }
    static int rr = 0;
    SubBuf &b = g_subbuf[rr++ % 32];
    b.out = out;
    return b.bytes;
}

// v36: the Frame's post-filter settings ride along at the end of every SLPW frame (48-byte "SLPS" trailer after the
// packets; Frame layers before v7.11 ignore bytes past the payload). See frame/decoder/slp_pyro_dec.hpp.
static void append_frame_settings(std::vector<uint8_t> &buf)
{
    static uint32_t gen = 0xffffffffu;
    static uint8_t tr[48];
    uint32_t g = slp_settings_generation();
    if (g != gen) {
        gen = g;
        memset(tr, 0, sizeof(tr));
        uint32_t magic = 0x53504c53u;   // "SLPS"
        uint16_t ver = 1, size = 48;
        // v38: deblock strength is capped at 1.0 (Mia, 2026-11-07: anything above looks worse whatever the other settings)
        auto db = [](double v) { return (float)std::min(1.0, std::max(0.0, v)); };
        float f[6] = {db(slp_setting("frame.deblock_wide", 0)), db(slp_setting("frame.deblock_inset", 0)),
                      (float)slp_setting("frame.sharpen", 0.5), (float)slp_setting("frame.sharpen_r0", 0.35),
                      (float)slp_setting("frame.sharpen_r1", 0.8), (float)slp_setting("frame.wide_step", 2)};
        uint8_t b[4] = {(uint8_t)(slp_setting("frame.deblock_mode", 0) > 0.5 ? 1 : 0),
                        (uint8_t)std::min(2.0, std::max(0.0, slp_setting("frame.deblock_coarse", 0) + 0.5)),
                        (uint8_t)(slp_setting("frame.deblock_bpp", 0) > 0.5 ? 1 : 0),
                        (uint8_t)(slp_setting("frame.postfx", 1) > 0.5 ? 1 : 0)};
        memcpy(tr, &magic, 4);
        memcpy(tr + 4, &ver, 2);
        memcpy(tr + 6, &size, 2);
        memcpy(tr + 8, &g, 4);
        memcpy(tr + 12, f, 24);
        memcpy(tr + 36, b, 4);
        // v44: wide-view blur on the Frame (layer v7.13+; older layers ignore these bytes). 0 px = off.
        float wb[2] = {(float)std::min(8.0, std::max(0.0, slp_setting("frame.wide_blur", 0))),
                       (float)std::min(3.0, std::max(0.0, slp_setting("frame.wide_blur_contrast", 1)))};
        memcpy(tr + 40, wb, 8);
        slp_log("[SET] Frame settings #%u: post-filter %s, deblock %s wide %.2f inset %.2f (grids %d, %s), sharpen %.2f (%.2f-%.2f)",
                g, b[3] ? "on" : "off", b[0] ? "smooth" : "seam", f[0], f[1], (int)b[1], b[2] ? "follows bits/pixel" : "fixed",
                f[2], f[3], f[4]);
        slp_log("[SET] Frame wide-view blur %.2f px, contrast %.2f", wb[0], wb[1]);
    }
    buf.insert(buf.end(), tr, tr + sizeof(tr));
}

// v45: per-frame "SLPR" block in front of the SLPS trailer: the shared edge ramp and where each inset is, so the
// Frame filters ramp around the same gaze point as the PC prefilter. Older Frame layers never look at it (they read
// only the last 48 bytes; Pyrowave ignores bytes after the packets). Layout (72 bytes, little endian):
//   0 "SLPR", 4 u16 version 1, 6 u16 size 72, 8 f32 ramp, start deg, end deg, contrast overshoot,
//   24 eye 0 f32 cx, cy, ax, bx, ay, by (tile units: pixel (x, y) looks along ((x - bx) / ax, (y - by) / ay)), 48 eye 1.
// v48: version 2, 80 bytes: + 72 f32 headset stability (0..1), 76 f32 its clamp width. Frame layers before v7.17
// look for version 1 only and ignore the block (no ramp); update both.
static void append_ramp(std::vector<uint8_t> &buf)
{
    SlpRampInfo r = slp_periph_ramp();
    uint8_t b[80] = {};
    uint32_t magic = 0x52504c53u;   // "SLPR"
    uint16_t ver = 2, size = 80;
    memcpy(b, &magic, 4);
    memcpy(b + 4, &ver, 2);
    memcpy(b + 6, &size, 2);
    float f[16] = {r.ramp, r.start, r.end, r.overshoot,
                   r.cx[0], r.cy[0], r.ax[0], r.bx[0], r.ay[0], r.by[0],
                   r.cx[1], r.cy[1], r.ax[1], r.bx[1], r.ay[1], r.by[1]};
    memcpy(b + 8, f, 64);
    float t[2] = {(float)std::min(1.0, std::max(0.0, slp_setting("frame.temporal", 0))),
                  (float)std::min(4.0, std::max(0.0, slp_setting("frame.temporal_clamp", 1)))};
    memcpy(b + 72, t, 8);
    buf.insert(buf.end(), b, b + sizeof(b));
}

static bool build_substitute_packet(uint64_t encode_index, std::vector<uint8_t> &buf)
{
    buf.clear();
    bool have = encode_index && slp_shadow_take_frame(encode_index, buf, 30);
    if (!have) {
        // "Repeat" frame: header only, num_packets = 0 -> the Frame re-presents its last picture.
        uint32_t hdr[8] = {0x57504c53u, 1u | (32u << 16), 0, 0, (uint32_t)encode_index, 0, 0, 0};
        buf.assign(reinterpret_cast<uint8_t *>(hdr), reinterpret_cast<uint8_t *>(hdr) + 32);
        g_sub_repeats.fetch_add(1);
    } else {
        g_sub_frames.fetch_add(1);
    }
    append_ramp(buf);
    append_frame_settings(buf);
    g_sub_bytes.fetch_add(buf.size());
    uint64_t total = g_sub_frames.load() + g_sub_repeats.load();
    if (total <= 3 || total % 720 == 0)
        slp_log("[SUB] frame #%llu: %s, %zu bytes (totals: %llu Pyrowave, %llu repeat)", (unsigned long long)encode_index,
                have ? "Pyrowave" : "REPEAT", buf.size(), (unsigned long long)g_sub_frames.load(),
                (unsigned long long)g_sub_repeats.load());
    return have;
}

bool slp_substitute_packet(uint64_t encode_index, std::vector<uint8_t> &buf)
{
    std::lock_guard<std::mutex> lk(g_sub_mutex);
    return build_substitute_packet(encode_index, buf);
}

static void substitute_bitstream(NV_ENC_LOCK_BITSTREAM *lb, uint64_t encode_index)
{
    std::lock_guard<std::mutex> lk(g_sub_mutex);
    std::vector<uint8_t> &buf = subbuf_for(lb->outputBitstream);
    build_substitute_packet(encode_index, buf);
    lb->bitstreamBufferPtr = buf.data();
    lb->bitstreamSizeInBytes = (uint32_t)buf.size();
}

static NVENCSTATUS NVENCAPI hook_LockBitstream(void *encoder, void *params)
{
    auto orig = reinterpret_cast<PFN_Generic2>(g_orig_lock.load());
    if (!orig) return NV_ENC_ERR_INVALID_CALL;

    double tl = now_ms();
    uint64_t n = g_lock_calls.fetch_add(1) + 1;
    uint64_t encode_index = 0;
    InFlight hit = {};
    if (params) {
        const void *out = static_cast<const NV_ENC_LOCK_BITSTREAM *>(params)->outputBitstream;
        std::lock_guard<std::mutex> lk(g_tm_mutex);
        sec_roll_locked(tl);
        for (auto &f : g_inflight)
            if (f.out == out && f.t > 0) {
                encode_index = f.n;
                hit = f;
                double d = tl - f.t;
                g_sec.n++; g_sec.sum += d;
                if (d > g_sec.max) { g_sec.max = d; g_sec.max_at = f.t - g_sec.win_start; g_sec.max_frame = f.n; }
                f.t = 0;
                break;
            }
    }
    NVENCSTATUS r;
    if (hit.bypassed && params) {
        // NVENC never saw this picture: fill the output fields vrlink reads ourselves.
        auto lb = static_cast<NV_ENC_LOCK_BITSTREAM *>(params);
        lb->frameIdx = hit.frame_idx;
        lb->hwEncodeStatus = 0;
        lb->numSlices = 1;
        lb->outputTimeStamp = hit.ts;
        lb->outputDuration = hit.dur;
        lb->pictureType = NV_ENC_PIC_TYPE_IDR;   // every Pyrowave frame is intra
        lb->pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        lb->frameAvgQP = 0;
        lb->frameSatd = 0;
        lb->ltrFrameIdx = 0;
        lb->ltrFrameBitmap = 0;
        {
            std::lock_guard<std::mutex> lk(g_bypass_mutex);
            g_bypassed_locked.insert(lb->outputBitstream);
        }
        r = NV_ENC_SUCCESS;
    } else {
        r = orig(encoder, params);
    }
    if (r == NV_ENC_SUCCESS && params && slp_substitute_active())
        substitute_bitstream(static_cast<NV_ENC_LOCK_BITSTREAM *>(params), encode_index);
    if (r == NV_ENC_SUCCESS && params)
        g_lock_bytes.fetch_add(static_cast<const NV_ENC_LOCK_BITSTREAM *>(params)->bitstreamSizeInBytes);
    if (r == NV_ENC_SUCCESS && params && (n <= 5 || n % 720 == 0)) {
        auto lb = static_cast<const NV_ENC_LOCK_BITSTREAM *>(params);
        slp_log(slp_substitute_active() ? "[SLP] NvEncLockBitstream #%llu: SUBSTITUTED bytes=%u" : "[SLP] NvEncLockBitstream #%llu: HEVC/NVENC bytes=%u",
                (unsigned long long)n, lb->bitstreamSizeInBytes);
    }
    return r;
}

static NVENCSTATUS NVENCAPI hook_UnlockBitstream(void *encoder, void *out)
{
    auto orig = reinterpret_cast<PFN_Generic2>(g_orig_unlock.load());
    if (!orig) return NV_ENC_ERR_INVALID_CALL;
    {
        std::lock_guard<std::mutex> lk(g_bypass_mutex);
        auto it = g_bypassed_locked.find(out);
        if (it != g_bypassed_locked.end()) { g_bypassed_locked.erase(it); return NV_ENC_SUCCESS; }
    }
    return orig(encoder, out);
}

// ---------------------------------------------------------------------------
// Function-table patching
// ---------------------------------------------------------------------------

static HMODULE module_of(const void *addr)
{
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(addr), &m);
    return m;
}

static void patch_function_list(void *list)
{
    auto base  = static_cast<uint8_t *>(list);
    auto slots = reinterpret_cast<void **>(base + kListHeaderBytes);

    // Sanity check: every populated slot 0..kCheckMax must live in one module
    // (the NVENC runtime).  If the layout assumption is wrong this fails and
    // we leave the table alone.
    HMODULE nv = module_of(slots[slot::OpenEncodeSession]);
    bool ok = nv && slots[slot::EncodePicture] && slots[slot::LockBitstream];
    for (size_t i = 0; ok && i <= slot::kCheckMax; ++i) {
        if (slots[i] && module_of(slots[i]) != nv) ok = false;
    }
    if (!ok) {
        slp_log("[SLP] function table sanity check FAILED - not patching (layout mismatch?)");
        return;
    }

    auto swap = [&](size_t idx, std::atomic<void *> &orig, void *hook) {
        void *cur = slots[idx];
        if (!cur || cur == hook) return;
        orig.store(cur);
        DWORD old = 0;
        VirtualProtect(&slots[idx], sizeof(void *), PAGE_READWRITE, &old);
        slots[idx] = hook;
        VirtualProtect(&slots[idx], sizeof(void *), old, &old);
    };

    swap(slot::InitializeEncoder,   g_orig_init,     reinterpret_cast<void *>(hook_InitializeEncoder));
    swap(slot::EncodePicture,       g_orig_encode,   reinterpret_cast<void *>(hook_EncodePicture));
    swap(slot::LockBitstream,       g_orig_lock,     reinterpret_cast<void *>(hook_LockBitstream));
    swap(slot::OpenEncodeSessionEx, g_orig_open_ex,  reinterpret_cast<void *>(hook_OpenEncodeSessionEx));
    swap(slot::RegisterResource,    g_orig_register, reinterpret_cast<void *>(hook_RegisterResource));
    swap(slot::MapInputResource,    g_orig_map,      reinterpret_cast<void *>(hook_MapInputResource));
    swap(slot::ReconfigureEncoder,  g_orig_reconf,   reinterpret_cast<void *>(hook_ReconfigureEncoder));
    swap(slot::UnlockBitstream,     g_orig_unlock,   reinterpret_cast<void *>(hook_UnlockBitstream));

    slp_log("[SLP] NVENC function table patched (observe-only)");
}

static NVENCSTATUS NVENCAPI hook_NvEncodeAPICreateInstance(void *list)
{
    auto orig = reinterpret_cast<PFN_CreateInstance>(g_orig_create.load());
    if (!orig) return NV_ENC_ERR_INVALID_CALL;
    NVENCSTATUS r = orig(list);
    slp_log("[SLP] NvEncodeAPICreateInstance called, status=%u", r);
    if (r == NV_ENC_SUCCESS && list) patch_function_list(list);
    return r;
}

static FARPROC WINAPI hook_GetProcAddress(HMODULE m, LPCSTR name)
{
    auto orig = reinterpret_cast<FARPROC(WINAPI *)(HMODULE, LPCSTR)>(g_orig_gpa.load());
    if (!orig) return nullptr;
    FARPROC r = orig(m, name);
    if (r && reinterpret_cast<uintptr_t>(name) > 0xFFFF &&
        std::strcmp(name, "NvEncodeAPICreateInstance") == 0 &&
        r != reinterpret_cast<FARPROC>(hook_NvEncodeAPICreateInstance))
    {
        g_orig_create.store(r);
        slp_log("[SLP] GetProcAddress(NvEncodeAPICreateInstance) intercepted");
        return reinterpret_cast<FARPROC>(hook_NvEncodeAPICreateInstance);
    }
    extern FARPROC slp_amf_hook_get_proc(HMODULE, LPCSTR, FARPROC);
    FARPROC amf = slp_amf_hook_get_proc(m, name, r);
    if (amf != r) return amf;
    return r;
}

// ---------------------------------------------------------------------------
// IAT patching
// ---------------------------------------------------------------------------

static bool istarts(const char *s, const char *prefix)
{
    return _strnicmp(s, prefix, std::strlen(prefix)) == 0;
}

// Patch imports named `proc` in `mod`.  `dll_ok` filters the exporting DLL name.
// First original value seen is stored in *orig_out.  Returns patched slot count.
template <typename DllFilter>
static int patch_module_import(HMODULE mod, const char *proc, FARPROC fn,
                               std::atomic<FARPROC> *orig_out, DllFilter dll_ok)
{
    auto base = reinterpret_cast<uint8_t *>(mod);
    auto dos  = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    const auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;

    int patched = 0;
    auto desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress);
    for (; desc->Name; ++desc) {
        const char *dll = reinterpret_cast<const char *>(base + desc->Name);
        if (!dll_ok(dll)) continue;
        if (!desc->OriginalFirstThunk) continue;

        auto name_thunk = reinterpret_cast<const IMAGE_THUNK_DATA *>(base + desc->OriginalFirstThunk);
        auto iat        = reinterpret_cast<FARPROC *>(base + desc->FirstThunk);
        for (; name_thunk->u1.AddressOfData; ++name_thunk, ++iat) {
            if (name_thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            auto ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(base + name_thunk->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char *>(ibn->Name), proc) != 0) continue;
            if (*iat == fn) continue; // already patched

            FARPROC expected = nullptr;
            orig_out->compare_exchange_strong(expected, *iat);

            DWORD old = 0;
            if (VirtualProtect(iat, sizeof(FARPROC), PAGE_READWRITE, &old)) {
                *iat = fn;
                VirtualProtect(iat, sizeof(FARPROC), old, &old);
                ++patched;
            }
        }
    }
    return patched;
}

// ---------------------------------------------------------------------------
// Module scan thread
// ---------------------------------------------------------------------------

static std::atomic<bool> g_stop{false};
static HANDLE            g_thread = nullptr;

static void scan_modules_once(std::set<HMODULE> &seen, const std::wstring_view windir)
{
    std::vector<HMODULE> mods(1024);
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods.data(),
                            (DWORD)(mods.size() * sizeof(HMODULE)), &needed))
        return;
    mods.resize(std::min<size_t>(needed / sizeof(HMODULE), mods.size()));

    std::set<HMODULE> now(mods.begin(), mods.end());
    for (auto it = seen.begin(); it != seen.end();)
        it = now.count(*it) ? std::next(it) : seen.erase(it);

    for (HMODULE m : mods) {
        if (m == g_self || seen.count(m)) continue;
        seen.insert(m);

        wchar_t path[MAX_PATH];
        if (!GetModuleFileNameW(m, path, MAX_PATH)) continue;
        // Skip system modules; we only care about Steam / SteamVR drivers.
        if (!windir.empty() && _wcsnicmp(path, windir.data(), windir.size()) == 0) continue;

        // Only vrlink's driver is touched (round-4 logs: it is the sole NVENC and
        // D3D11CreateDevice user that matters).  Earlier builds patched every
        // non-system module (Steam overlay, SDL2, other drivers); no reason to.
        {
            const wchar_t *leafw = std::wcsrchr(path, L'\\');
            leafw = leafw ? leafw + 1 : path;
            if (_wcsicmp(leafw, L"driver_vrlink.dll") != 0) continue;
        }
        int a = patch_module_import(m, "NvEncodeAPICreateInstance",
            reinterpret_cast<FARPROC>(hook_NvEncodeAPICreateInstance), &g_orig_create,
            [](const char *d) { return istarts(d, "nvencodeapi"); });
        int b = patch_module_import(m, "GetProcAddress",
            reinterpret_cast<FARPROC>(hook_GetProcAddress), &g_orig_gpa,
            [](const char *d) { return istarts(d, "kernel32") || istarts(d, "kernelbase") ||
                                       istarts(d, "api-ms-win-core-libraryloader"); });
        int c = patch_module_import(m, "D3D11CreateDevice",
            reinterpret_cast<FARPROC>(hook_D3D11CreateDevice), &g_orig_create_device,
            [](const char *d) { return istarts(d, "d3d11"); });
        extern FARPROC slp_amf_init_hook_proc();
        extern void slp_amf_hook_set_init_original(FARPROC);
        int d = 0;
        if (temp_flag_exists("slp_amf_off")) {
            if (!g_amf_off_logged.exchange(true))
                slp_log("[AMF] interception disabled by %%TEMP%%\\slp_amf_off");
        } else {
            d = patch_module_import(m, "AMFInit", slp_amf_init_hook_proc(), &g_orig_amf_init,
                [](const char *dll) { return istarts(dll, "amfrt64"); });
            if (d) slp_amf_hook_set_init_original(g_orig_amf_init.load());
        }
        if (c) {
            char narrow[MAX_PATH];
            WideCharToMultiByte(CP_ACP, 0, path, -1, narrow, MAX_PATH, nullptr, nullptr);
            const char *leaf = std::strrchr(narrow, '\\');
            slp_log("[SLP] patched D3D11CreateDevice import in %s", leaf ? leaf + 1 : narrow);
        }
        if (a || b || d) {
            char narrow[MAX_PATH];
            WideCharToMultiByte(CP_ACP, 0, path, -1, narrow, MAX_PATH, nullptr, nullptr);
            const char *leaf = std::strrchr(narrow, '\\');
            slp_log("[SLP] patched imports in %s: CreateInstance=%d GetProcAddress=%d AMFInit=%d",
                    leaf ? leaf + 1 : narrow, a, b, d);
        }
    }
}

static DWORD WINAPI scan_thread(LPVOID)
{
    wchar_t wd[MAX_PATH];
    UINT n = GetWindowsDirectoryW(wd, MAX_PATH);
    std::wstring windir(wd, n);

    std::set<HMODULE> seen;
    while (!g_stop.load()) {
        scan_modules_once(seen, windir);
        Sleep(500);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Public install / remove (called from DllMain in SteamFramePyrowaveBackend.cpp)
// ---------------------------------------------------------------------------

bool slp_nvenc_hook_install()
{
    // Resolve our own module handle for the scan's self-skip.
    g_self = module_of(reinterpret_cast<const void *>(&slp_nvenc_hook_install));

    g_stop.store(false);
    g_thread = CreateThread(nullptr, 0, scan_thread, nullptr, 0, nullptr);
    if (!g_thread) {
        slp_log("[SLP] could not start module scan thread");
        return false;
    }
    slp_log("[SLP] NVENC/AMF hooks armed; waiting for a stream to start");
    return true;
}

void slp_nvenc_hook_remove()
{
    // Do not join: this is called under the loader lock.  The scan thread exits
    // on its own; hooks stay patched (restoring them under load is unsafe), and
    // the process is expected to be exiting.
    g_stop.store(true);
    slp_log("[SLP] hook remove (detach)");
}
