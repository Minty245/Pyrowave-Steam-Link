// Steam Link Pyrowave - user settings (PC).
// SPDX-License-Identifier: MIT
//
// One JSON file, %LOCALAPPDATA%\SteamLinkPyrowave\settings.json, written by the settings app (or by hand) and re-read
// live (within ~0.5 s of a change). Sections: general, foveation, weights, frame (the Frame's post-filter, sent to the
// headset inside the stream). On first run the file is created from the old %TEMP%\slp_*.txt files if present,
// else from the built-in defaults. Logs and calibration data live in the same folder.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Data folder with trailing backslash (created on first use), e.g. C:\Users\x\AppData\Local\SteamLinkPyrowave\ .
const std::string &slp_data_dir();
std::string slp_data_path(const char *name);
// %TEMP%\name (ANSI-safe spelling; empty if there is none). v52.
std::string slp_temp_path(const char *name);

// Re-reads settings.json if it changed (at most every 0.5 s) and returns a counter that increases on every change.
uint32_t slp_settings_generation();
// "section.key" -> number (true/false -> 1/0). def if missing.
double slp_setting(const char *key, double def);
// All numeric keys of one section ("foveation" -> {"fov", 118}, ...).
std::vector<std::pair<std::string, double>> slp_settings_section(const char *section);
