// Pyrowave Link settings app - Windows entry point.
//   PyrowaveLink.exe            desktop window
//   PyrowaveLink.exe --overlay  SteamVR dashboard overlay (started by SteamVR through the app manifest)
// SPDX-License-Identifier: MIT
#include "RmlUi_Include_GL3.h"
#include "RmlUi_Renderer_GL3.h"
#include "RmlUi_Platform_Win32.h"

#include <RmlUi/Core.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <openvr.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "settings_model.h"
#include "ui.h"

namespace fs = std::filesystem;

namespace {

const char *kAppKey = "pyrowave.link.settings";
const wchar_t *kWndClass = L"PyrowaveLinkWindow";

fs::path exe_dir()
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(std::wstring(buf, n)).parent_path();
}

void log_line(const char *fmt, ...)
{
    static fs::path p = slp::data_dir() / "logs" / "app.log";
    FILE *f = _wfopen(p.c_str(), L"a");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "%02d:%02d:%02d ", st.wHour, st.wMinute, st.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

class Log : public SystemInterface_Win32 {
public:
    bool LogMessage(Rml::Log::Type type, const Rml::String &message) override
    {
        if (type <= Rml::Log::LT_WARNING) log_line("[rml] %s", message.c_str());
        return true;
    }
};

void open_path(const std::string &utf8)
{
    std::wstring w = RmlWin32::ConvertToUTF16(utf8);
    HINSTANCE r = ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + w + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

// ---- OpenGL 3.3 core context on a window ----
typedef HGLRC(WINAPI *PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int *);
typedef BOOL(WINAPI *PFN_wglSwapIntervalEXT)(int);

bool create_gl(HWND hwnd, HDC *out_dc, HGLRC *out_rc)
{
    HDC dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cStencilBits = 8;
    int pf = ChoosePixelFormat(dc, &pfd);
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) return false;
    HGLRC tmp = wglCreateContext(dc);
    if (!tmp || !wglMakeCurrent(dc, tmp)) return false;
    auto create = (PFN_wglCreateContextAttribsARB)wglGetProcAddress("wglCreateContextAttribsARB");
    HGLRC rc = nullptr;
    if (create) {
        const int attrs[] = {0x2091, 3, 0x2092, 3, 0x9126, 0x00000001, 0};   // 3.3 core
        rc = create(dc, nullptr, attrs);
    }
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(tmp);
    if (!rc || !wglMakeCurrent(dc, rc)) return false;
    if (auto swap = (PFN_wglSwapIntervalEXT)wglGetProcAddress("wglSwapIntervalEXT")) swap(1);
    *out_dc = dc;
    *out_rc = rc;
    return true;
}

float window_scale(HWND hwnd)
{
    typedef UINT(WINAPI * PFN_GetDpiForWindow)(HWND);
    static auto fn = (PFN_GetDpiForWindow)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    UINT dpi = fn && hwnd ? fn(hwnd) : 96;
    return dpi / 96.0f;
}

void enable_dpi_awareness()
{
    typedef BOOL(WINAPI * PFN_SetCtx)(HANDLE);
    if (auto fn = (PFN_SetCtx)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))
        if (fn((HANDLE)-4)) return;   // per-monitor v2
    SetProcessDPIAware();
}

// ---- SteamVR registration (desktop process) ----
fs::path write_manifest()
{
    fs::path exe = exe_dir() / L"PyrowaveLink.exe";
    fs::path man = slp::data_dir() / L"pyrowave_link.vrmanifest";
    std::string bin = exe.u8string();
    for (char &c : bin)
        if (c == '\\') c = '/';
    std::ofstream f(man, std::ios::binary | std::ios::trunc);
    f << "{\n  \"source\": \"builtin\",\n  \"applications\": [{\n"
         "    \"app_key\": \"" << kAppKey << "\",\n"
         "    \"launch_type\": \"binary\",\n"
         "    \"binary_path_windows\": \"" << bin << "\",\n"
         "    \"arguments\": \"--overlay\",\n"
         "    \"is_dashboard_overlay\": true,\n"
         "    \"strings\": { \"en_us\": { \"name\": \"Pyrowave Link\", \"description\": \"Pyrowave streaming settings\" } }\n"
         "  }]\n}\n";
    return man;
}

// v2.6: is vrserver.exe running? VR_Init, even as a background app, can bring SteamVR up (SteamVR 2.19 with Steam
// Link), so the app only talks to SteamVR once it is already running.
bool steamvr_running()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        found = _wcsicmp(pe.szExeFile, L"vrserver.exe") == 0;
    CloseHandle(snap);
    return found;
}

// Returns true once SteamVR was reachable and the registration matches `want`.
bool sync_steamvr_registration(bool want)
{
    if (!steamvr_running()) return false;   // try again later; never start SteamVR from here
    vr::EVRInitError err = vr::VRInitError_None;
    vr::IVRSystem *sys = vr::VR_Init(&err, vr::VRApplication_Background);
    if (!sys || err != vr::VRInitError_None) return false;   // SteamVR not running: try again later
    vr::IVRApplications *apps = vr::VRApplications();
    fs::path man = write_manifest();
    std::string m = man.u8string();
    if (want) {
        vr::EVRApplicationError e = apps->AddApplicationManifest(m.c_str(), false);
        apps->SetApplicationAutoLaunch(kAppKey, true);
        if (apps->GetApplicationProcessId(kAppKey) == 0) apps->LaunchApplication(kAppKey);
        log_line("SteamVR: dashboard overlay registered (%d)", (int)e);
    } else {
        apps->SetApplicationAutoLaunch(kAppKey, false);
        apps->RemoveApplicationManifest(m.c_str());
        log_line("SteamVR: dashboard overlay unregistered");
    }
    vr::VR_Shutdown();
    return true;
}

// ---- desktop window ----
struct Desktop {
    HWND hwnd = nullptr;
    HDC dc = nullptr;
    HGLRC rc = nullptr;
    Rml::Context *ctx = nullptr;
    RenderInterface_GL3 *ren = nullptr;
    TextInputMethodEditor_Win32 ime;
    bool redraw = true;
    int w = 0, h = 0;
};
Desktop *g_desk = nullptr;

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Desktop *d = g_desk;
    switch (msg) {
    case WM_CLOSE: PostQuitMessage(0); return 0;
    case WM_SIZE:
        if (d && d->ctx) {
            d->w = LOWORD(lp);
            d->h = HIWORD(lp);
            d->ctx->SetDimensions({d->w, d->h});
            d->ren->SetViewport(d->w, d->h);
            d->redraw = true;
        }
        return 0;
    case WM_DPICHANGED:
        if (d && d->ctx) {
            d->ctx->SetDensityIndependentPixelRatio(HIWORD(wp) / 96.0f);
            const RECT *r = (const RECT *)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        float s = window_scale(hwnd);
        auto *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = (LONG)(900 * s);
        mm->ptMinTrackSize.y = (LONG)(600 * s);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        if (d) d->redraw = true;
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    }
    if (d && d->ctx) {
        d->redraw = true;
        if (!RmlWin32::WindowProcedure(d->ctx, d->ime, hwnd, msg, wp, lp)) return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int run_desktop(HINSTANCE inst)
{
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\PyrowaveLinkDesktop");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(kWndClass, nullptr)) {
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = kWndClass;
    RegisterClassExW(&wc);

    Desktop d;
    g_desk = &d;
    d.hwnd = CreateWindowExW(0, kWndClass, L"Pyrowave Link", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
                             nullptr, nullptr, inst, nullptr);
    if (!d.hwnd) return 1;
    float s = window_scale(d.hwnd);
    RECT r = {0, 0, (LONG)(1180 * s), (LONG)(780 * s)};
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
    SetWindowPos(d.hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(d.hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    COLORREF cap = RGB(0x0b, 0x0b, 0x0f);
    DwmSetWindowAttribute(d.hwnd, 35 /* DWMWA_CAPTION_COLOR, Windows 11 */, &cap, sizeof(cap));

    if (!create_gl(d.hwnd, &d.dc, &d.rc)) {
        MessageBoxW(d.hwnd, L"OpenGL 3.3 is not available on this PC.", L"Pyrowave Link", MB_ICONERROR);
        return 1;
    }
    Rml::String msg;
    if (!RmlGL3::Initialize(&msg)) {
        MessageBoxW(d.hwnd, L"Could not load OpenGL.", L"Pyrowave Link", MB_ICONERROR);
        return 1;
    }
    Log sys;
    sys.SetWindow(d.hwnd);
    static RenderInterface_GL3 ren;   // static: destroyed after RmlUi
    d.ren = &ren;
    Rml::SetSystemInterface(&sys);
    Rml::SetRenderInterface(&ren);
    Rml::SetTextInputHandler(&d.ime);
    Rml::Initialise();
    std::string assets = (exe_dir() / L"assets").u8string();
    slp::App::load_fonts(assets);
    RECT cr;
    GetClientRect(d.hwnd, &cr);
    d.w = cr.right;
    d.h = cr.bottom;
    d.ctx = Rml::CreateContext("main", {d.w, d.h});
    d.ctx->SetDensityIndependentPixelRatio(s);
    ren.SetViewport(d.w, d.h);

    bool want_dash = true;
    slp::PlatformHooks hooks;
    hooks.open_path = open_path;
    double next_vr = 0;
    bool vr_done = false;
    hooks.dashboard_changed = [&](bool on) { want_dash = on; vr_done = false; next_vr = 0; };
    {
        slp::App app(slp::Mode::Desktop, d.ctx, assets, hooks);
        if (!app.ok()) {
            MessageBoxW(d.hwnd, L"The interface files are missing (assets folder next to PyrowaveLink.exe).", L"Pyrowave Link", MB_ICONERROR);
            return 1;
        }
        want_dash = slp::Settings(slp::data_dir()).get("app.dashboard") > 0.5;
        ShowWindow(d.hwnd, SW_SHOWNORMAL);
        log_line("desktop started (%s)", msg.c_str());

        bool running = true;
        while (running) {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                if (m.message == WM_QUIT) running = false;
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            if (!running) break;
            if (app.tick()) d.redraw = true;
            double now = slp::now_seconds();
            if (!vr_done && now >= next_vr) {
                next_vr = now + 5.0;
                vr_done = sync_steamvr_registration(want_dash);
            }
            if (d.redraw && d.w > 0 && d.h > 0) {
                d.redraw = false;
                app.update_context();
                glViewport(0, 0, d.w, d.h);
                ren.BeginFrame();
                ren.Clear();
                d.ctx->Render();
                ren.EndFrame();
                SwapBuffers(d.dc);
            } else {
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
            }
        }
        app.tick();   // write pending edits
    }
    Rml::Shutdown();
    ReleaseMutex(mutex);
    return 0;
}

// ---- SteamVR dashboard overlay ----
int run_overlay(HINSTANCE inst, bool flip)
{
    vr::EVRInitError err = vr::VRInitError_None;
    vr::VR_Init(&err, vr::VRApplication_Overlay);
    if (err != vr::VRInitError_None) {
        log_line("overlay: VR_Init failed: %s", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        return 1;
    }
    vr::IVROverlay *ov = vr::VROverlay();
    vr::VROverlayHandle_t main = 0, thumb = 0;
    vr::EVROverlayError oe = ov->CreateDashboardOverlay(kAppKey, "Pyrowave Link", &main, &thumb);
    if (oe != vr::VROverlayError_None) {
        log_line("overlay: CreateDashboardOverlay failed: %s (already running?)", ov->GetOverlayErrorNameFromEnum(oe));
        vr::VR_Shutdown();
        return 1;
    }
    std::string thumb_png = (exe_dir() / L"assets" / L"logo.png").u8string();
    ov->SetOverlayFromFile(thumb, thumb_png.c_str());
    ov->SetOverlayWidthInMeters(main, 2.25f);   // v2.6: 1.5x (Mia: a little small at 1.5 m)
    ov->SetOverlayInputMethod(main, vr::VROverlayInputMethod_Mouse);

    // GL context on a hidden window
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = L"PyrowaveLinkOverlayGL";
    RegisterClassW(&wc);
    HWND hidden = CreateWindowW(wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    HDC dc;
    HGLRC rc;
    Rml::String msg;
    if (!hidden || !create_gl(hidden, &dc, &rc) || !RmlGL3::Initialize(&msg)) {
        log_line("overlay: no OpenGL 3.3");
        vr::VR_Shutdown();
        return 1;
    }
    Log sys;
    static RenderInterface_GL3 ren;
    Rml::SetSystemInterface(&sys);
    Rml::SetRenderInterface(&ren);
    Rml::Initialise();
    std::string assets = (exe_dir() / L"assets").u8string();
    slp::App::load_fonts(assets);

    const float kScale = 2.0f;                       // texture pixels per dp
    const int W = (int)(960 * kScale), Hmax = (int)(720 * kScale);
    Rml::Context *ctx = Rml::CreateContext("overlay", {W, Hmax});
    ctx->SetDensityIndependentPixelRatio(kScale);

    GLuint tex = 0, fbo = 0;
    int H = 0;
    auto alloc = [&](int h) {
        if (h == H) return;
        H = h;
        if (!tex) glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        if (!fbo) glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        ctx->SetDimensions({W, H});
        ren.SetViewport(W, H);
        vr::HmdVector2_t scale = {(float)W, (float)H};
        ov->SetOverlayMouseScale(main, &scale);
        if (flip) {
            vr::VRTextureBounds_t b = {0, 1, 1, 0};
            ov->SetOverlayTextureBounds(main, &b);
        }
    };

    slp::PlatformHooks hooks;
    hooks.open_path = open_path;
    slp::App app(slp::Mode::Overlay, ctx, assets, hooks);
    if (!app.ok()) {
        log_line("overlay: interface files missing");
        vr::VR_Shutdown();
        return 1;
    }
    // v1.2: the texture keeps one size for the whole session. v1.1 resized it to fit each tab, which made SteamVR
    // move the overlay and left it showing a stale picture (re-specifying a GL texture SteamVR already shares).
    // All tabs have four rows; size the texture to the tallest one.
    float tallest = 0;
    for (const char *id : {"stream", "headset", "foveation"}) {
        app.show(id);
        app.update_context();
        if (Rml::ElementDocument *doc = ctx->GetDocument(0))
            if (Rml::Element *root = doc->GetElementById("root"))
                tallest = std::max(tallest, root->GetBox().GetSize(Rml::BoxArea::Margin).y);
    }
    alloc(std::max(160, std::min(Hmax, tallest > 0 ? (int)std::ceil(tallest) + 1 : (int)(652 * kScale))));
    ren.SetOutputFramebuffer(fbo);
    log_line("overlay started (%s), texture %dx%d", msg.c_str(), W, H);
    slp::Settings watch(slp::data_dir());

    bool running = true, dirty = true;
    double next_check = 0;
    while (running) {
        vr::VREvent_t e;
        while (vr::VRSystem()->PollNextEvent(&e, sizeof(e)))
            if (e.eventType == vr::VREvent_Quit) {
                vr::VRSystem()->AcknowledgeQuit_Exiting();
                running = false;
            }
        while (ov->PollNextOverlayEvent(main, &e, sizeof(e))) {
            dirty = true;
            switch (e.eventType) {
            case vr::VREvent_MouseMove: {
                // v1.1: SteamVR reports y top-down for this overlay (v1 flipped it and input came out upside down)
                float y = flip ? H - e.data.mouse.y : e.data.mouse.y;
                ctx->ProcessMouseMove((int)e.data.mouse.x, (int)y, 0);
                break;
            }
            case vr::VREvent_MouseButtonDown:
                ctx->ProcessMouseButtonDown(0, 0);
                break;
            case vr::VREvent_MouseButtonUp:
                ctx->ProcessMouseButtonUp(0, 0);
                break;
            case vr::VREvent_OverlayHidden:
                ctx->ProcessMouseLeave();
                break;
            default: break;
            }
        }
        if (app.tick()) dirty = true;
        double now = slp::now_seconds();
        if (now >= next_check) {   // switched off in the desktop app
            next_check = now + 1.0;
            watch.poll();
            if (watch.get("app.dashboard") < 0.5) running = false;
        }
        bool visible = ov->IsOverlayVisible(main);
        if (visible && dirty) {
            dirty = false;
            app.update_context();
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glViewport(0, 0, W, H);
            ren.BeginFrame();
            ren.Clear();
            ctx->Render();
            ren.EndFrame();
            glFlush();
            vr::Texture_t t = {(void *)(uintptr_t)tex, vr::TextureType_OpenGL, vr::ColorSpace_Auto};
            ov->SetOverlayTexture(main, &t);
        }
        Sleep(visible ? 11 : 100);
    }
    app.tick();
    Rml::Shutdown();
    vr::VR_Shutdown();
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
{
    enable_dpi_awareness();
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool overlay = false, flip = false;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--overlay")) overlay = true;
        if (!wcscmp(argv[i], L"--flip")) flip = true;
    }
    LocalFree(argv);
    return overlay ? run_overlay(inst, flip) : run_desktop(inst);
}
