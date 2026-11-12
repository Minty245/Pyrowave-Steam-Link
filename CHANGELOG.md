# Changelog

## 1.0.0 - first public release

- PC: SteamVR driver + backend (v54): Pyrowave in place of NVENC HEVC for Steam Link, foveated prefilter with gaze,
  region-weighted rate control, safety brake, full colour (4:4:4) option; Steam Link's own 4:2:0 conversion steps
  (sharper chroma downsample, luma adjustment) and a peripheral colour blur that keeps true brightness; a log of
  vrlink's NVENC configuration and QP maps; ANSI-safe data paths for non-ASCII user folders.
- Frame: Vulkan layer + decoder (v7.22): GPU Pyrowave decoding per stream (HEVC streams untouched), headset filters,
  full colour into the compositor with compressed 10-bit images, default output mode 1, lower GPU work per frame (filters
  folded into the full-colour pack, faster blur pyramid).
- Pyrowave Link 2.7: one default tune with per-setting Default / Mine A/B buttons, Pyrowave on/off in the header,
  bigger dashboard overlay, Sharper colour, Luma adjustment and "Colour blur keeps
  brightness" toggles.
- Installers: PC (no admin, Settings > Apps entry), Frame (user space only), optional SSH push to the Frame.
