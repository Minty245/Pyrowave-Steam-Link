// Steam Link Pyrowave - settings.json keys and built-in defaults, shared by the backend and the settings app.
// SPDX-License-Identifier: MIT
// v52: defaults = Mia's settings of 2026-11-11 (safety brake 0 / recovery 100 ms, wide weight 1, middle boost 1,
// periphery contrast 1.5, headset stability 0.5, stability clamp 0.9); simple_scene_saving 0 and contrast_overshoot 0 (both retired from the app, held at these values).
// v54 defaults = Mia's tune of 2026-11-12 (768 stream): blur strength 0.5, contrast restore 3, colour blur 1.5, keep
// refining calibration on, edge ramp 0.3, wide views 1.5, headset blur 0.1 / contrast 3, headset stability off.
// v54: + foveation.chroma_luma (on). v53: + general.sharp_chroma, general.luma_adjust (on: Steam Link's 4:2:0 conversion steps).
// v50: + general.full_colour (off). v49 defaults = the Balanced preset = Mia's 200 Mbps tune of 2026-11-09.
// Each entry: {"section.key", default}. Order = order in a newly written settings.json.
{"general.enabled", 1}, {"general.multiplier", 1.0}, {"general.simple_scene_saving", 0}, {"general.full_colour", 0}, {"general.sharp_chroma", 1}, {"general.luma_adjust", 1}, {"general.safety_brake", 0}, {"general.brake_recovery_ms", 100},
    {"foveation.enable", 1}, {"foveation.fov", 118}, {"foveation.safe", 2}, {"foveation.ramp", 3},
    {"foveation.strength", 0.5}, {"foveation.max_lod", 2}, {"foveation.contrast", 3}, {"foveation.contrast_clamp", 1},
    {"foveation.fovea_blur", 1}, {"foveation.wide_min_lod", 0}, {"foveation.chroma_lod", 1.5}, {"foveation.chroma_luma", 1},
    {"foveation.inset_scale", 0.29}, {"foveation.csf", 1}, {"foveation.display_cpd", 11},
    {"foveation.gaze", 1}, {"foveation.gaze_now", 0}, {"foveation.gaze_delay_ms", -1},
    {"foveation.gaze_max_age_ms", 250}, {"foveation.locator", 1},
    {"foveation.hide", 0}, {"foveation.hide_margin", 0.75}, {"foveation.hide_lod", 6}, {"foveation.hide_window_ms", 12},
    {"foveation.lens_deg", 0}, {"foveation.lens_feather_deg", 4}, {"foveation.safe_unlocated", 30},
    {"foveation.edge_ramp", 0.3}, {"foveation.edge_start", 15}, {"foveation.edge_end", 55},
    {"foveation.contrast_overshoot", 0}, {"foveation.true_angles", 1},
    {"foveation.temporal", 0}, {"foveation.temporal_clamp", 0.6}, {"foveation.cpd0", 30}, {"foveation.e2", 2.5}, {"foveation.log", 1},
    {"weights.fovea", 1}, {"weights.wide", 1.5}, {"weights.centre_boost", 1}, {"weights.centre_radius", 0.5},
    {"weights.under_inset", 0.1}, {"weights.inset_edge_boost", 0}, {"weights.inset_edge_width", 0.04}, {"weights.inset_shown", 0.45},
    {"frame.postfx", 1}, {"frame.deblock_wide", 0}, {"frame.deblock_inset", 0}, {"frame.deblock_mode", 0},
    {"frame.deblock_coarse", 0}, {"frame.deblock_bpp", 0}, {"frame.sharpen", 0.5}, {"frame.sharpen_r0", 0.35},
    {"frame.sharpen_r1", 0.8}, {"frame.wide_step", 2},
    {"frame.wide_blur", 0.1}, {"frame.wide_blur_contrast", 3}, {"frame.temporal", 0}, {"frame.temporal_clamp", 0.9},
    {"app.dashboard", 1},
