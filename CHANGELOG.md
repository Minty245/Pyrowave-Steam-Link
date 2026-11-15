# Changelog

## Unreleased (1.1.0)

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
