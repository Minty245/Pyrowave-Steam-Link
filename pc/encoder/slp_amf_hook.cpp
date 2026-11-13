// Steam Link Pyrowave - AMF component hook.
// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "slp_shadow.h"

void slp_log(const char *fmt, ...);
bool slp_substitute_packet(uint64_t encode_index, std::vector<uint8_t> &out);
void slp_nvenc_get_shadow_source(ID3D11DeviceContext **ctx_out, ID3D11Texture2D **atlas_out);

namespace {

using AmfResult = int32_t;

constexpr AmfResult kAmfOk = 0;
constexpr int32_t kAmfMemoryHost = 1;
constexpr int32_t kAmfMemoryDx11 = 3;
constexpr int32_t kAmfDataSurface = 1;
constexpr int32_t kAmfVariantInt64 = 2;
// Slot indices follow AMD's public AMF interface declarations; no AMF runtime
// or SDK headers are linked into this DLL.
constexpr size_t kFactoryVtableSize = 7;
constexpr size_t kComponentVtableSize = 28;

namespace factory_slot {
constexpr size_t CreateComponent = 1;
}

namespace context_slot {
constexpr size_t LockDX11 = 20;
constexpr size_t UnlockDX11 = 21;
constexpr size_t AllocBuffer = 43;
}

namespace data_slot {
constexpr size_t CopyTo = 10;
constexpr size_t GetMemoryType = 13;
constexpr size_t GetDataType = 17;
constexpr size_t SetPts = 19;
constexpr size_t GetPts = 20;
constexpr size_t SetDuration = 21;
constexpr size_t GetDuration = 22;
constexpr size_t Release = 1;
}

namespace surface_slot {
constexpr size_t GetPlaneAt = 25;
}

namespace plane_slot {
constexpr size_t GetNative = 4;
}

namespace buffer_slot {
constexpr size_t GetSize = 24;
constexpr size_t GetNative = 25;
}

namespace component_slot {
constexpr size_t SetProperty = 3;
constexpr size_t SubmitInput = 22;
constexpr size_t QueryOutput = 23;
constexpr size_t GetContext = 24;
}

struct AmfVariant {
    int32_t type;
    union {
        int64_t int64_value;
        uint8_t raw[16];
    } value;
};
static_assert(sizeof(AmfVariant) == 24, "AMFVariantStruct layout mismatch");

enum class InterfaceKind {
    Factory,
    Component
};

struct HookRecord {
    InterfaceKind kind;
    bool frame_substitution = false;
    std::vector<void *> original;
    std::vector<void *> replacement;
    struct PendingFrame {
        uint64_t index;
        int64_t pts;
    };
    std::deque<PendingFrame> frames;
};

struct ComponentVtableRecord {
    std::vector<void *> original;
    bool set_property_patched = false;
    bool submit_input_patched = false;
    bool query_output_patched = false;
};

std::mutex g_hooks_mutex;
std::unordered_map<void *, HookRecord> g_hooks;
std::unordered_map<void **, ComponentVtableRecord> g_component_vtables;
std::atomic<uint64_t> g_frame_index{0};
std::atomic<uint64_t> g_query_output_calls{0};
std::atomic<bool> g_warned_no_atlas{false};
std::atomic<bool> g_warned_dx11_lock{false};
std::atomic<FARPROC> g_amf_init_original{nullptr};

template <typename Fn>
Fn original_method(void *object, size_t slot)
{
    std::lock_guard<std::mutex> lock(g_hooks_mutex);
    auto it = g_hooks.find(object);
    if (it != g_hooks.end() && slot < it->second.original.size())
        return reinterpret_cast<Fn>(it->second.original[slot]);
    if (!object) return nullptr;
    auto **vtable = *reinterpret_cast<void ***>(object);
    auto shared = g_component_vtables.find(vtable);
    if (shared == g_component_vtables.end() || slot >= shared->second.original.size()) return nullptr;
    return reinterpret_cast<Fn>(shared->second.original[slot]);
}

bool component_frame_substitution_enabled(void *component)
{
    std::lock_guard<std::mutex> lock(g_hooks_mutex);
    auto it = g_hooks.find(component);
    return it != g_hooks.end() && it->second.kind == InterfaceKind::Component &&
           it->second.frame_substitution;
}

void register_component_if_hooked(void *component)
{
    if (!component) return;
    auto **vtable = *reinterpret_cast<void ***>(component);
    if (!vtable) return;
    std::lock_guard<std::mutex> lock(g_hooks_mutex);
    auto shared = g_component_vtables.find(vtable);
    if (shared == g_component_vtables.end() || g_hooks.find(component) != g_hooks.end()) return;
    HookRecord record = {};
    record.kind = InterfaceKind::Component;
    record.original = shared->second.original;
    g_hooks.emplace(component, std::move(record));
}

template <typename Fn>
Fn vtable_method(void *object, size_t slot)
{
    if (!object) return nullptr;
    auto **vtable = *reinterpret_cast<void ***>(object);
    if (!vtable || !vtable[slot]) return nullptr;
    return reinterpret_cast<Fn>(vtable[slot]);
}

bool patch_interface(void *object, InterfaceKind kind, size_t count,
                     const std::vector<std::pair<size_t, void *>> &hooks)
{
    if (!object) return false;
    std::lock_guard<std::mutex> lock(g_hooks_mutex);
    if (g_hooks.find(object) != g_hooks.end()) return true;

    void ***vptr = reinterpret_cast<void ***>(object);
    void **current = *vptr;
    if (!current) return false;

    HookRecord record = {};
    record.kind = kind;
    record.original.assign(current, current + count);
    record.replacement.resize(count + 1);
    // MSVC stores the RTTI complete-object locator immediately before the vtable.
    record.replacement[0] = current[-1];
    std::copy(record.original.begin(), record.original.end(), record.replacement.begin() + 1);
    for (const auto &hook : hooks) {
        if (hook.first >= count) return false;
        record.replacement[hook.first + 1] = hook.second;
    }

    DWORD old_protection = 0;
    if (!VirtualProtect(vptr, sizeof(*vptr), PAGE_READWRITE, &old_protection)) return false;
    auto inserted = g_hooks.emplace(object, std::move(record));
    if (!inserted.second) {
        DWORD ignored = 0;
        VirtualProtect(vptr, sizeof(*vptr), old_protection, &ignored);
        return true;
    }
    *vptr = inserted.first->second.replacement.data() + 1;
    DWORD ignored = 0;
    VirtualProtect(vptr, sizeof(*vptr), old_protection, &ignored);
    return true;
}

bool patch_component(void *component);

AmfResult __stdcall hook_factory_create_component(void *self, void *context, const wchar_t *id,
                                                   void **out_component)
{
    using Fn = AmfResult (__stdcall *)(void *, void *, const wchar_t *, void **);
    Fn original = original_method<Fn>(self, factory_slot::CreateComponent);
    if (!original) return static_cast<AmfResult>(-1);
    char component_id[128] = "(null)";
    if (id) WideCharToMultiByte(CP_UTF8, 0, id, -1, component_id, sizeof(component_id), nullptr, nullptr);
    slp_log("[AMF] Factory::CreateComponent enter id=%s", component_id);
    AmfResult result = original(self, context, id, out_component);
    slp_log("[AMF] Factory::CreateComponent exit id=%s result=0x%08x component=%p",
            component_id, static_cast<uint32_t>(result), out_component ? *out_component : nullptr);
    if (result == kAmfOk && out_component && *out_component)
        register_component_if_hooked(*out_component);
    const bool is_hevc_encoder = id &&
        (std::wcscmp(id, L"AMFVideoEncoderVCE_HEVC") == 0 ||
         std::wcscmp(id, L"AMFVideoEncoderHW_HEVC") == 0 ||
         std::wcscmp(id, L"AMFVideoEncoder_HEVC") == 0);
    if (result == kAmfOk && is_hevc_encoder && out_component && *out_component) {
        if (temp_flag_exists("slp_amf_no_component")) {
            slp_log("[AMF] HEVC component wrapping bypassed by %%TEMP%%\\slp_amf_no_component: %s",
                    component_id);
        } else if (patch_component(*out_component)) {
            slp_log("[AMF] HEVC encoder component wrapped: %s", component_id);
        }
    }
    return result;
}

void patch_factory(void *factory)
{
    const std::vector<std::pair<size_t, void *>> hooks = {
        {factory_slot::CreateComponent, reinterpret_cast<void *>(hook_factory_create_component)}
    };
    if (!patch_interface(factory, InterfaceKind::Factory, kFactoryVtableSize, hooks))
        slp_log("[AMF] factory vtable patch failed");
}

bool get_surface_texture(void *data, ID3D11Texture2D **texture)
{
    if (!data || !texture) return false;
    *texture = nullptr;
    auto **vtable = *reinterpret_cast<void ***>(data);
    using GetMemoryTypeFn = int32_t (__stdcall *)(void *);
    using GetDataTypeFn = int32_t (__stdcall *)(void *);
    if (!vtable || reinterpret_cast<GetDataTypeFn>(vtable[data_slot::GetDataType])(data) != kAmfDataSurface ||
        reinterpret_cast<GetMemoryTypeFn>(vtable[data_slot::GetMemoryType])(data) != kAmfMemoryDx11)
        return false;

    using GetPlaneFn = void * (__stdcall *)(void *, size_t);
    auto *plane = reinterpret_cast<GetPlaneFn>(vtable[surface_slot::GetPlaneAt])(data, 0);
    if (!plane) return false;
    auto **plane_vtable = *reinterpret_cast<void ***>(plane);
    using GetNativeFn = void * (__stdcall *)(void *);
    *texture = static_cast<ID3D11Texture2D *>(
        reinterpret_cast<GetNativeFn>(plane_vtable[plane_slot::GetNative])(plane));
    return *texture != nullptr;
}

int64_t data_pts(void *data)
{
    auto **vtable = *reinterpret_cast<void ***>(data);
    using Fn = int64_t (__stdcall *)(void *);
    return reinterpret_cast<Fn>(vtable[data_slot::GetPts])(data);
}

void *component_context(void *component)
{
    using Fn = void * (__stdcall *)(void *);
    Fn get_context = original_method<Fn>(component, component_slot::GetContext);
    return get_context ? get_context(component) : nullptr;
}

AmfResult __stdcall hook_component_submit_input(void *self, void *data)
{
    using Fn = AmfResult (__stdcall *)(void *, void *);
    Fn original = original_method<Fn>(self, component_slot::SubmitInput);
    if (!original) return static_cast<AmfResult>(-1);
    if (!component_frame_substitution_enabled(self)) return original(self, data);

    uint64_t frame_index = 0;
    int64_t pts = 0;
    ID3D11Texture2D *submitted = nullptr;
    if (get_surface_texture(data, &submitted)) {
        pts = data_pts(data);
        ID3D11DeviceContext *context = nullptr;
        ID3D11Texture2D *atlas = nullptr;
        slp_nvenc_get_shadow_source(&context, &atlas);
        if (!atlas) {
            D3D11_TEXTURE2D_DESC desc = {};
            submitted->GetDesc(&desc);
            if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                atlas = submitted;
                atlas->AddRef();
            }
        }
        if (!atlas && !g_warned_no_atlas.exchange(true))
            slp_log("[AMF] original RGBA16F atlas was not discovered; keeping AMF output");
        if (context && atlas) {
            frame_index = g_frame_index.fetch_add(1, std::memory_order_relaxed) + 1;
            void *amf_context = component_context(self);
            using LockFn = AmfResult (__stdcall *)(void *);
            using UnlockFn = AmfResult (__stdcall *)(void *);
            LockFn lock_dx11 = vtable_method<LockFn>(amf_context, context_slot::LockDX11);
            UnlockFn unlock_dx11 = vtable_method<UnlockFn>(amf_context, context_slot::UnlockDX11);
            bool locked = amf_context && lock_dx11 && lock_dx11(amf_context) == kAmfOk;
            if (locked) {
                slp_shadow_frame(context, atlas, submitted, frame_index, nullptr);
                if (unlock_dx11) unlock_dx11(amf_context);
            } else {
                frame_index = 0;
                if (!g_warned_dx11_lock.exchange(true))
                    slp_log("[AMF] DX11 context lock unavailable; leaving this output on AMF");
            }
        }
        if (atlas) atlas->Release();
        if (context) context->Release();
    }

    AmfResult result = original(self, data);
    if (result == kAmfOk && frame_index) {
        std::lock_guard<std::mutex> lock(g_hooks_mutex);
        auto it = g_hooks.find(self);
        if (it != g_hooks.end()) it->second.frames.push_back({frame_index, pts});
    }
    return result;
}

bool create_output_buffer(void *context, const std::vector<uint8_t> &bytes, void **out_buffer)
{
    if (!context || !out_buffer) return false;
    *out_buffer = nullptr;
    using AllocBufferFn = AmfResult (__stdcall *)(void *, int32_t, size_t, void **);
    AllocBufferFn alloc_buffer = vtable_method<AllocBufferFn>(context, context_slot::AllocBuffer);
    if (!alloc_buffer) return false;

    void *buffer = nullptr;
    AmfResult result = alloc_buffer(context, kAmfMemoryHost, bytes.size(), &buffer);
    if (result != kAmfOk || !buffer) {
        if (buffer) {
            auto **vtable = *reinterpret_cast<void ***>(buffer);
            using ReleaseFn = int32_t (__stdcall *)(void *);
            reinterpret_cast<ReleaseFn>(vtable[data_slot::Release])(buffer);
        }
        return false;
    }

    auto **vtable = *reinterpret_cast<void ***>(buffer);
    using GetSizeFn = size_t (__stdcall *)(void *);
    using GetNativeFn = void * (__stdcall *)(void *);
    size_t size = reinterpret_cast<GetSizeFn>(vtable[buffer_slot::GetSize])(buffer);
    void *native = reinterpret_cast<GetNativeFn>(vtable[buffer_slot::GetNative])(buffer);
    if (!native || size < bytes.size()) {
        using ReleaseFn = int32_t (__stdcall *)(void *);
        reinterpret_cast<ReleaseFn>(vtable[data_slot::Release])(buffer);
        return false;
    }
    std::memcpy(native, bytes.data(), bytes.size());
    *out_buffer = buffer;
    return true;
}

void copy_data_timing(void *source, void *target)
{
    auto **source_vtable = *reinterpret_cast<void ***>(source);
    auto **target_vtable = *reinterpret_cast<void ***>(target);
    using GetTimeFn = int64_t (__stdcall *)(void *);
    using SetTimeFn = void (__stdcall *)(void *, int64_t);
    auto get_pts = reinterpret_cast<GetTimeFn>(source_vtable[data_slot::GetPts]);
    auto get_duration = reinterpret_cast<GetTimeFn>(source_vtable[data_slot::GetDuration]);
    auto set_pts = reinterpret_cast<SetTimeFn>(target_vtable[data_slot::SetPts]);
    auto set_duration = reinterpret_cast<SetTimeFn>(target_vtable[data_slot::SetDuration]);
    set_pts(target, get_pts(source));
    set_duration(target, get_duration(source));
}

bool copy_output_properties(void *source, void *target)
{
    static std::atomic<bool> logged_failure{false};
    static std::atomic<bool> logged_success{false};
    if (!source || !target) return false;
    auto **source_vtable = *reinterpret_cast<void ***>(source);
    using CopyToFn = AmfResult (__stdcall *)(void *, void *, bool);
    auto copy_to = reinterpret_cast<CopyToFn>(source_vtable[data_slot::CopyTo]);
    if (!copy_to) return false;
    AmfResult result = copy_to(source, target, true);
    if (result != kAmfOk) {
        if (!logged_failure.exchange(true))
            slp_log("[AMF] could not copy AMF output properties to replacement (result=0x%08x)",
                    static_cast<uint32_t>(result));
        return false;
    }
    if (!logged_success.exchange(true))
        slp_log("[AMF] copied AMF output properties to replacement");
    return true;
}

AmfResult __stdcall hook_component_query_output(void *self, void **out_data)
{
    using Fn = AmfResult (__stdcall *)(void *, void **);
    Fn original = original_method<Fn>(self, component_slot::QueryOutput);
    if (!original) return static_cast<AmfResult>(-1);
    if (!component_frame_substitution_enabled(self)) return original(self, out_data);

    const uint64_t call_index = g_query_output_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (temp_flag_exists("slp_amf_query_passthrough")) {
        if (call_index <= 8)
            slp_log("[AMF] QueryOutput #%llu passthrough enter",
                    static_cast<unsigned long long>(call_index));
        AmfResult result = original(self, out_data);
        if (call_index <= 8)
            slp_log("[AMF] QueryOutput #%llu passthrough result=0x%08x data=%p",
                    static_cast<unsigned long long>(call_index), static_cast<uint32_t>(result),
                    out_data ? *out_data : nullptr);
        return result;
    }
    if (call_index <= 8)
        slp_log("[AMF] QueryOutput #%llu enter self=%p out_data=%p",
                static_cast<unsigned long long>(call_index), self, out_data);
    AmfResult result = original(self, out_data);
    if (call_index <= 8)
        slp_log("[AMF] QueryOutput #%llu returned result=0x%08x data=%p",
                static_cast<unsigned long long>(call_index), static_cast<uint32_t>(result),
                out_data ? *out_data : nullptr);
    if (result != kAmfOk || !out_data || !*out_data) return result;

    int64_t pts = data_pts(*out_data);
    uint64_t frame_index = 0;
    {
        std::lock_guard<std::mutex> lock(g_hooks_mutex);
        auto record = g_hooks.find(self);
        if (record != g_hooks.end() && !record->second.frames.empty()) {
            auto &frames = record->second.frames;
            auto match = std::find_if(frames.begin(), frames.end(), [pts](const HookRecord::PendingFrame &frame) {
                return frame.pts == pts;
            });
            if (match == frames.end()) match = frames.begin();
            frame_index = match->index;
            frames.erase(match);
        }
    }
    if (call_index <= 8)
        slp_log("[AMF] QueryOutput #%llu pts=%lld frame=%llu",
                static_cast<unsigned long long>(call_index), static_cast<long long>(pts),
                static_cast<unsigned long long>(frame_index));
    if (!frame_index || !slp_substitute_active()) return result;

    std::vector<uint8_t> bytes;
    if (!slp_substitute_packet(frame_index, bytes)) {
        if (call_index <= 8)
            slp_log("[AMF] QueryOutput #%llu no SLP packet; keeping AMF output",
                    static_cast<unsigned long long>(call_index));
        return result;
    }
    if (call_index <= 8)
        slp_log("[AMF] QueryOutput #%llu substituting %zu bytes",
                static_cast<unsigned long long>(call_index), bytes.size());
    void *buffer = nullptr;
    if (!create_output_buffer(component_context(self), bytes, &buffer)) {
        if (call_index <= 8)
            slp_log("[AMF] QueryOutput #%llu output allocation failed; keeping AMF output",
                    static_cast<unsigned long long>(call_index));
        return result;
    }

    if (!copy_output_properties(*out_data, buffer)) {
        auto **buffer_vtable = *reinterpret_cast<void ***>(buffer);
        using ReleaseFn = int32_t (__stdcall *)(void *);
        reinterpret_cast<ReleaseFn>(buffer_vtable[data_slot::Release])(buffer);
        return result;
    }
    auto **old_vtable = *reinterpret_cast<void ***>(*out_data);
    copy_data_timing(*out_data, buffer);
    using ReleaseFn = int32_t (__stdcall *)(void *);
    reinterpret_cast<ReleaseFn>(old_vtable[data_slot::Release])(*out_data);
    *out_data = buffer;
    if (call_index <= 8)
        slp_log("[AMF] QueryOutput #%llu substituted output returned",
                static_cast<unsigned long long>(call_index));
    return result;
}

AmfResult __stdcall hook_component_set_property(void *self, const wchar_t *name, AmfVariant value)
{
    using Fn = AmfResult (__stdcall *)(void *, const wchar_t *, AmfVariant);
    Fn original = original_method<Fn>(self, component_slot::SetProperty);
    if (!original) return static_cast<AmfResult>(-1);

    AmfResult result = original(self, name, value);
    if (result != kAmfOk || !name || value.type != kAmfVariantInt64) return result;

    if (std::wcscmp(name, L"HevcTargetBitrate") == 0 && value.value.int64_value > 0) {
        slp_vrlink_amf_target_bps(static_cast<uint64_t>(value.value.int64_value));
    } else if (std::wcscmp(name, L"HevcPeakBitrate") == 0 && value.value.int64_value > 0) {
        static std::atomic<int64_t> last_peak_bps{0};
        const int64_t peak_bps = value.value.int64_value;
        if (last_peak_bps.exchange(peak_bps) != peak_bps)
            slp_log("[AMF] HEVC peak bitrate %.1f Mbps", peak_bps / 1e6);
    }
    return result;
}

bool patch_component(void *component)
{
    const bool hook_submit_input = !temp_flag_exists("slp_amf_no_submit");
    const bool hook_query_output = !temp_flag_exists("slp_amf_no_query");
    if (!hook_submit_input && !hook_query_output) {
        slp_log("[AMF] both component hooks bypassed by diagnostic flags");
        return true;
    }

    if (!component) return false;
    void **vtable = *reinterpret_cast<void ***>(component);
    if (!vtable) return false;

    std::lock_guard<std::mutex> lock(g_hooks_mutex);
    auto object_record = g_hooks.find(component);
    if (object_record != g_hooks.end() && object_record->second.kind == InterfaceKind::Component) {
        object_record->second.frame_substitution = true;
        return true;
    }

    auto vtable_record = g_component_vtables.find(vtable);
    if (vtable_record == g_component_vtables.end()) {
        ComponentVtableRecord record = {};
        record.original.assign(vtable, vtable + kComponentVtableSize);
        auto inserted = g_component_vtables.emplace(vtable, std::move(record));
        if (!inserted.second) return false;
        vtable_record = inserted.first;
    }

    const bool hook_set_property = hook_submit_input || hook_query_output;
    const bool patch_set_property = hook_set_property && !vtable_record->second.set_property_patched;
    const bool patch_submit = hook_submit_input && !vtable_record->second.submit_input_patched;
    const bool patch_query = hook_query_output && !vtable_record->second.query_output_patched;
    if (patch_set_property || patch_submit || patch_query) {
        const size_t first_slot = patch_set_property ? component_slot::SetProperty :
            (patch_submit ? component_slot::SubmitInput : component_slot::QueryOutput);
        const size_t last_slot = patch_query ? component_slot::QueryOutput :
            (patch_submit ? component_slot::SubmitInput : component_slot::SetProperty);
        DWORD old_protection = 0;
        if (!VirtualProtect(vtable + first_slot, (last_slot - first_slot + 1) * sizeof(void *),
                            PAGE_READWRITE, &old_protection)) {
            if (vtable_record->second.original.size() == kComponentVtableSize &&
                !vtable_record->second.set_property_patched &&
                !vtable_record->second.submit_input_patched &&
                !vtable_record->second.query_output_patched)
                g_component_vtables.erase(vtable_record);
            slp_log("[AMF] component vtable protection change failed");
            return false;
        }
        if (patch_set_property) {
            vtable[component_slot::SetProperty] = reinterpret_cast<void *>(hook_component_set_property);
            vtable_record->second.set_property_patched = true;
        }
        if (patch_submit) {
            vtable[component_slot::SubmitInput] = reinterpret_cast<void *>(hook_component_submit_input);
            vtable_record->second.submit_input_patched = true;
        }
        if (patch_query) {
            vtable[component_slot::QueryOutput] = reinterpret_cast<void *>(hook_component_query_output);
            vtable_record->second.query_output_patched = true;
        }
        DWORD ignored = 0;
        if (!VirtualProtect(vtable + first_slot, (last_slot - first_slot + 1) * sizeof(void *),
                            old_protection, &ignored))
            slp_log("[AMF] component vtable protection restore failed");
    }

    HookRecord record = {};
    record.kind = InterfaceKind::Component;
    record.frame_substitution = true;
    record.original = vtable_record->second.original;
    g_hooks.emplace(component, std::move(record));
    slp_log("[AMF] component methods hooked in shared vtable: SetProperty=%d SubmitInput=%d QueryOutput=%d",
            hook_set_property, hook_submit_input, hook_query_output);
    return true;
}

AmfResult __cdecl hook_amf_init(uint64_t version, void **out_factory)
{
    using Fn = AmfResult (__cdecl *)(uint64_t, void **);
    Fn original = reinterpret_cast<Fn>(g_amf_init_original.load(std::memory_order_acquire));
    if (!original) return static_cast<AmfResult>(-1);
    AmfResult result = original(version, out_factory);
    if (result == kAmfOk && out_factory && *out_factory) {
        patch_factory(*out_factory);
        slp_log("[AMF] AMFInit intercepted");
    }
    return result;
}

bool is_amf_runtime(HMODULE module)
{
    if (!module) return false;
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return false;
    const wchar_t *leaf = std::wcsrchr(path, L'\\');
    leaf = leaf ? leaf + 1 : path;
    return _wcsicmp(leaf, L"amfrt64.dll") == 0;
}

} // namespace

FARPROC slp_amf_init_hook_proc()
{
    return reinterpret_cast<FARPROC>(hook_amf_init);
}

void slp_amf_hook_set_init_original(FARPROC original)
{
    if (original && original != slp_amf_init_hook_proc())
        g_amf_init_original.store(original, std::memory_order_release);
}

FARPROC slp_amf_hook_get_proc(HMODULE module, LPCSTR name, FARPROC resolved)
{
    if (!resolved || !name || reinterpret_cast<uintptr_t>(name) <= 0xFFFF ||
        std::strcmp(name, "AMFInit") != 0 || !is_amf_runtime(module))
        return resolved;
    if (temp_flag_exists("slp_amf_off")) return resolved;
    slp_amf_hook_set_init_original(resolved);
    slp_log("[AMF] GetProcAddress(AMFInit) intercepted");
    return slp_amf_init_hook_proc();
}
