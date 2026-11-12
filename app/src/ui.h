// Pyrowave Link settings app - the UI (RmlUi data model shared by the desktop window and the SteamVR overlay).
// SPDX-License-Identifier: MIT
#pragma once
#include <functional>
#include <memory>
#include <string>

namespace Rml { class Context; }

namespace slp {

enum class Mode { Desktop, Overlay };

struct PlatformHooks {
    std::function<void(const std::string &path_utf8)> open_path;   // folder or file in Explorer / default app
    std::function<void(bool on)> dashboard_changed;                 // "Show in SteamVR dashboard" toggled
};

class App {
public:
    // assets_dir: folder with app.rcss, main.rml, overlay.rml, fonts/.
    static bool load_fonts(const std::string &assets_dir);
    App(Mode mode, Rml::Context *ctx, const std::string &assets_dir, PlatformHooks hooks);
    ~App();
    bool ok() const;

    // Call every loop iteration: re-reads files (rate-limited), writes pending edits. Returns true if something
    // visible changed (so a render is due).
    bool tick();
    // Wrap Context::Update with this so changes RmlUi makes while syncing bindings are not taken as user edits.
    void update_context();

    // Preview / tests: switch page or tab by id ("stream", "foveation", "headset", "colour", "advanced").
    void show(const std::string &id);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace slp
