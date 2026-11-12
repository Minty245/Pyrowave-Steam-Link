// Pyrowave Link settings app - Linux preview harness (GLFW + GL3), used to screenshot and test the UI under Xvfb.
//   slp_preview --mode desktop|overlay [--show id] [--size WxH] [--scale s] [--click x,y]... [--drag x0,y0,x1,y1] --out f.ppm
// SPDX-License-Identifier: MIT
#include <RmlUi/Core.h>
#include <RmlUi/Debugger.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <string>
#include <vector>

#include "RmlUi_Renderer_GL3.h"
#include "RmlUi_Include_GL3.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "RmlUi_Platform_GLFW.h"
#include "ui.h"

int main(int argc, char **argv)
{
    std::string mode = "desktop", show, out = "out.ppm", assets = "assets";
    int w = 1180, h = 780;
    float scale = 1.0f;
    struct Act { char kind; float a, b, c, d; };
    std::vector<Act> acts;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--mode") mode = next();
        else if (a == "--show") show = next();
        else if (a == "--out") out = next();
        else if (a == "--assets") assets = next();
        else if (a == "--size") sscanf(next().c_str(), "%dx%d", &w, &h);
        else if (a == "--scale") scale = (float)atof(next().c_str());
        else if (a == "--click") { Act x{'c'}; sscanf(next().c_str(), "%f,%f", &x.a, &x.b); acts.push_back(x); }
        else if (a == "--drag") { Act x{'d'}; sscanf(next().c_str(), "%f,%f,%f,%f", &x.a, &x.b, &x.c, &x.d); acts.push_back(x); }
    }
    if (!glfwInit()) { fprintf(stderr, "glfwInit failed\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    int pw = (int)(w * scale), ph = (int)(h * scale);
    GLFWwindow *win = glfwCreateWindow(pw, ph, "preview", nullptr, nullptr);
    if (!win) { fprintf(stderr, "window failed\n"); return 1; }
    glfwMakeContextCurrent(win);
    Rml::String msg;
    if (!RmlGL3::Initialize(&msg)) { fprintf(stderr, "GL3: %s\n", msg.c_str()); return 1; }
    fprintf(stderr, "%s\n", msg.c_str());

    SystemInterface_GLFW sys;
    RenderInterface_GL3 ren;
    Rml::SetSystemInterface(&sys);
    Rml::SetRenderInterface(&ren);
    Rml::Initialise();
    if (!slp::App::load_fonts(assets)) fprintf(stderr, "font load failed\n");
    Rml::Context *ctx = Rml::CreateContext("main", Rml::Vector2i(pw, ph));
    ctx->SetDensityIndependentPixelRatio(scale);
    ren.SetViewport(pw, ph);

    // Offscreen target, to exercise the same path as the OpenVR overlay.
    GLuint fbo = 0, tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ren.SetOutputFramebuffer(fbo);

    slp::PlatformHooks hooks;
    hooks.open_path = [](const std::string &p) { fprintf(stderr, "[open] %s\n", p.c_str()); };
    hooks.dashboard_changed = [](bool on) { fprintf(stderr, "[dashboard] %d\n", on); };
    slp::App app(mode == "overlay" ? slp::Mode::Overlay : slp::Mode::Desktop, ctx, assets, hooks);
    if (!app.ok()) { fprintf(stderr, "document failed to load\n"); return 1; }
    if (!show.empty()) app.show(show);

    auto frame = [&]() {
        app.tick();
        app.update_context();
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ren.BeginFrame();
        ren.Clear();
        ctx->Render();
        ren.EndFrame();
    };
    for (int i = 0; i < 3; ++i) frame();
    for (const Act &a : acts) {
        float s = scale;
        if (a.kind == 'c') {
            ctx->ProcessMouseMove((int)(a.a * s), (int)(a.b * s), 0);
            frame();
            ctx->ProcessMouseButtonDown(0, 0);
            ctx->ProcessMouseButtonUp(0, 0);
        } else {
            ctx->ProcessMouseMove((int)(a.a * s), (int)(a.b * s), 0);
            frame();
            ctx->ProcessMouseButtonDown(0, 0);
            for (int k = 1; k <= 8; ++k) {
                ctx->ProcessMouseMove((int)((a.a + (a.c - a.a) * k / 8) * s), (int)((a.b + (a.d - a.b) * k / 8) * s), 0);
                frame();
            }
            ctx->ProcessMouseButtonUp(0, 0);
        }
        for (int i = 0; i < 3; ++i) frame();
    }
    ctx->ProcessMouseMove(-100, -100, 0);
    for (int i = 0; i < 3; ++i) frame();

    int crop = ph;   // overlay: only the content height, like the real overlay texture
    if (mode == "overlay")
        if (Rml::Element *root = ctx->GetDocument(0)->GetElementById("root"))
            crop = std::min(ph, (int)std::ceil(root->GetBox().GetSize(Rml::BoxArea::Margin).y) + 1);
    std::vector<unsigned char> px((size_t)pw * ph * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glReadPixels(0, 0, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    FILE *f = fopen(out.c_str(), "wb");
    fprintf(f, "P6\n%d %d\n255\n", pw, crop);
    for (int y = ph - 1; y >= ph - crop; --y)
        for (int x = 0; x < pw; ++x) fwrite(&px[((size_t)y * pw + x) * 4], 1, 3, f);
    fclose(f);
    // let debounced writes land
    double t0 = glfwGetTime();
    while (glfwGetTime() - t0 < 0.3) frame();
    Rml::Shutdown();
    _exit(0);   // the GL3 renderer must not be destroyed after the GL loader is gone
    glfwTerminate();
    return 0;
}
