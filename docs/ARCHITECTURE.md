# How Steam Link Pyrowave works

Steam Link already renders a foveated **atlas** on the PC: four square views stacked vertically (per eye a sharp
gaze-following *inset* and a *wide* view), encodes it with NVENC HEVC, sends it, and on the Frame decodes it with the
hardware decoder and composes the views. Steam Link Pyrowave replaces only the codec and leaves the rest of that
pipeline to Steam Link.

## PC (`pc/`)

- **`pc/driver/driver_slp`** - a SteamVR driver with no devices. SteamVR loads it into `vrserver.exe`; it loads
  `SteamFramePyrowaveBackend.dll` next to it and hands it SteamVR's driver-input interface (for the gaze). Off switch
  for testing: `%TEMP%\slp_driver_disabled`.
- **`pc/encoder`** - the backend.
  - `slp_nvenc_hook.cpp`: patches `driver_vrlink.dll`'s imports of `NvEncodeAPICreateInstance` / `D3D11CreateDevice`
    (only that module) and replaces entries of the NVENC function table. With Pyrowave on, NVENC is bypassed: the
    hooks answer vrlink's EncodePicture / LockBitstream / Reconfigure calls, so vrlink's pacing, FEC and stats see a
    normal encoder. vrlink's live bitrate target sets the budget.
  - `slp_periph.cpp` + `slp_periph.hlsl` / `slp_pyr.hlsl`: the foveated prefilter on vrlink's D3D11 device - a Gaussian
    pyramid of the atlas, per-pixel blur from the angle to the gaze (acuity model, the edge ramp, colour-only blur that keeps each
    pixel's linear luminance),
    contrast restoration. Gaze from `slp_gaze.cpp` (SteamVR driver input, calibrated against the picture).
  - `slp_shadow.cpp`: shares the atlas with a Vulkan device (shared texture + fence) and encodes it with Pyrowave
    (4:2:0, or 4:4:4 with full colour) with the fork's region weights and per-tile CSF. For 4:2:0 the fork can do the
    RGB to YCbCr step like Steam Link's HEVC path: an 8-tap chroma downsample clamped to each view, and luma
    adjustment (luma solved so the decoded pixel keeps the source's luminance). Output: one **SLPW frame** per
    picture (32-byte header, packet size table, packets) plus two trailers the Frame reads: `SLPR` (edge ramp, inset
    positions, stability) and `SLPS` (headset filter settings).
  - Safety brake: after vrlink reports a lost frame, the part of the bitrate above 1.0x vrlink's target is trimmed for
    a moment and recovers over `general.brake_recovery_ms`.
  - `slp_settings.cpp` / `slp_status.cpp`: `%LOCALAPPDATA%\SteamLinkPyrowave\settings.json` (read live) and
    `status.json` (for the app). Logs in `logs\backend.log`.
- **`pc/common`** - the settings defaults and the small JSON reader shared with the app.

## Frame (`frame/`)

- **`frame/layer`** - `libslp_layer.so`, an implicit Vulkan layer (installed in `~/.local`). It does nothing except in
  two processes:
  - **vrlink** (with `~/.local/lib/slp/AUTO`): it stands in for the V4L2 hardware decoder (`slp_emu.inc.c`). vrlink
    keeps talking V4L2; the layer answers. The first frame of each stream decides: SLPW frames are decoded with
    Pyrowave on the GPU into vrlink's own NV12 surfaces, anything else goes to the real hardware decoder. A decoder
    vrlink opened before the layer loaded is adopted (v7.20).
  - **vrcompositor** (with full colour enabled): when the compositor imports one of vrlink's stream surfaces, the layer
    asks vrlink over a private socket for the matching 10-bit RGBA image and gives the compositor a view of that
    instead. Barriers are mirrored onto it; if anything fails, the NV12 surface stays in use.
- **`frame/decoder`** - `libslp_pyrodec.so`, the GPU decoder behind the layer: Pyrowave decode, the headset filters
  (seam deblock, off by default; contrast-preserving wide-view blur with a stability history, inset sharpening), and the output
  (8-bit NV12 copies, or for full colour one pass that packs and filters into a compressed A2B10G10R10 image).
  `slp_selftest` checks every output path against a CPU reference on the Frame.
- **`frame/tools`** - `collect_stall.sh` (read-only diagnostics, redacted) and `power_probe.sh` (battery power and GPU
  clocks during a stream).

## Settings app (`app/`)

Pyrowave Link (RmlUi on OpenGL): a desktop window and a SteamVR dashboard overlay (`--overlay`), both editing
`settings.json`. The app registers the overlay with SteamVR while it runs (`app.dashboard`).

## Settings flow

`settings.json` -> backend (live). Headset filter settings travel inside the stream (`SLPS` / `SLPR`), so the Frame
needs no configuration. Frame-only switches are files in `~/.local/lib/slp`: `AUTO`, `DISABLED`, `FULLCOLOUR`, `MODE`,
`NODITHER`, `RGBA_LINEAR`, `NO_MERGE`, `EXACT_PYRAMID` (see the scripts in `tools/`).
