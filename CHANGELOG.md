# Changelog

## 1.1.0 - 2026-11-17

Headset support for HEVC full colour from [Steam Link Full Colour](https://github.com/Minty245/Steam-Link-Full-Colour)
(install the Frame package with `--full-colour`), installer fixes, and like-for-like latency numbers in the README
(Pyrowave matches Steam Link's HEVC).

- Frame (layer v7.33, decoder 7.33): HEVC full colour does less work per frame. The rebuild covers only the
  picture's rows (the rows below stay black from the clear at import: a quarter of the work in 4:2:2, half in 4:4:4),
  and the shared images use the GPU's compressed tiling (UBWC) where the driver allows, as Pyrowave's full colour has
  since v7.22 (`RGBA_LINEAR` turns it off). `power_probe.sh` also keeps a fixed-name copy of its result
  (`power-latest.txt`).
- Frame (layer v7.32, decoder 7.32): HEVC full colour layout 3, the tiled 4:4:4 of Steam Link Full Colour 0.2.0 (Cr in
  64x64 blocks whose quarters sit in the chroma samples over the block, so reds are coded at their own area's foveation
  QP; less red noise in the sharp centre). Layout 2 (Full Colour 0.1.x) still works. `unpack_test` covers both.
- Frame (layer v7.31, decoder 7.31): HEVC full colour at any stream width. The Frame's decoder pads its surfaces (1280
  wide decodes into 1344, 704 into 768, 6832 tall into 6848); v7.30 only rebuilt packed pictures that filled the surface
  exactly and showed the others as plain 4:2:0 (scrambled colour away from 768 wide). The unpacker now reads the
  picture inside the padded surface (padding repeats the last column) and the layer logs a packed picture that does not
  fit. `unpack_test` takes optional padding (`pad_w pad_h`).
- `collect_stall.sh` also collects the kernel's video / GPU messages and where each vrlink / vrcompositor thread is
  waiting, and keeps a fixed-name copy of the newest archive (`stall-latest.tar.gz`).
- Install-Frame.ps1: `-Package <file or folder>` installs a specific Frame package (a folder: its newest one).
- Frame (layer v7.30): HEVC full colour keeps working across 4:2:2 / 4:4:4 switches. Steam Link reallocates its frame
  surfaces at every switch and the driver reuses their handles; the layer's tables kept the old entries, so a new 4:4:4
  surface could be described with an old 4:2:2 size, vrcompositor refused the picture, and frames flashed between full
  colour and the packed picture. Reused handles now replace their entries. `fc_test.c` covers it.
- Frame (layer v7.29): vrcompositor swaps in the rebuilt picture for 10-bit stream surfaces too (HEVC Main10, as Steam
  Link sends it); before, only 8-bit (Pyrowave) views were swapped, so HEVC full colour was unpacked but never shown.
  `fc_test.c` runs with 10-bit surfaces when `SLP_FC_10BIT=1`.
- Frame (layer v7.28): HEVC full colour now works when vrlink opened the decoder before the layer loaded (the usual
  case). The layer keeps its own read-only view of the stream buffers for such sessions (released when vrlink
  reallocates them), so the marker is seen and pictures are rebuilt. In v7.26 these sessions were never watched and
  vrcompositor showed the packed picture as plain 4:2:0; v7.27 looked the buffers up in the process map, which does not
  work (the driver reports every buffer at offset 0). The log now says when the stream can't be read or has no marker.
- Frame (layer / decoder v7.26): HEVC full colour for Steam Link Full Colour (a separate PC driver). While full colour
  is armed, stock HEVC sessions are watched for its in-band marker; vrlink's decoder surfaces are imported read-only,
  and each decoded packed 4:2:2 / 4:4:4 picture is rebuilt into the shared 10-bit image vrcompositor samples (the same
  handover as Pyrowave's full colour). Stream surfaces may now be up to 8 tiles tall. Host test:
  `frame/decoder/unpack_test.cpp`.
- Frame (layer v7.25): image barriers cover every mip level and layer of the blur pyramid and the stability history
  (before, only the first). Edge smoothing and motion-compensated stability were tried (v7.23/v7.24) and removed: no
  visible gain in the headset, and the smoothing looked worse.
- Installers: skip Steam libraries that no longer exist; the uninstaller finds SteamVR in other libraries; both Frame
  install routes offer to reboot the Frame at the end; `tools/shutdown_probe.sh` for unclean-shutdown reports.

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
