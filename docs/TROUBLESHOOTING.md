# Troubleshooting

**Connected, but no picture (or a frozen one).**
- Is the headset part installed and was SteamVR restarted on the Frame afterwards? Without it the Frame's hardware
  decoder receives Pyrowave frames it cannot decode. Check `/tmp/slp_layer/` on the Frame: a `vrlink-*.log` with
  "Pyrowave decoder ... ready" means it is working.
- *Full colour* on in Pyrowave Link but not enabled on the Frame gives no picture: switch it off in the app, or run
  `./install.sh --full-colour` on the Frame and restart SteamVR there.
- If it happens now and then: run `~/.local/lib/slp/tools/collect_stall.sh` on the Frame while it is stuck and attach
  the archive to a bug report (addresses and IDs are redacted).

**Steam Link streams HEVC instead.** *Pyrowave streaming* must be on in Pyrowave Link, and the change applies when the
stream restarts. `%LOCALAPPDATA%\SteamLinkPyrowave\logs\backend.log` shows whether the backend loaded
(`[SHADOW] ready: SUBSTITUTION mode`). An NVIDIA GPU is required.

**Stutters or blocky frames under load.** Lower the Steam Link bandwidth target or improve the wireless link. The
safety brake (Stream page) trims the bitrate above 1.0x for a moment after a lost frame.

**Dropped or late frames at large stream sizes.** The Frame decodes and filters every frame on its GPU, which shares
the time with Steam Link and the compositor. At 144 Hz (6.9 ms per frame) that runs out above an encoded video size of
about 1000 px: at 1344 the decode and headset filters alone take about 7.3 ms. Use a smaller size (768 is the tested
one, and the default tune is made for it), or a lower refresh rate for larger sizes.

**Higher battery use than HEVC.** Expected (see the README). Full colour and 4:4:4 add about 1 W each.

**Turn things off without uninstalling.**
- PC: switch *Pyrowave streaming* off (HEVC after a stream restart), or create `%TEMP%\slp_driver_disabled` before
  starting SteamVR.
- Frame: `touch ~/.local/lib/slp/DISABLED`, restart SteamVR there.

**Frame self-test.** The Frame package has `selftest/run_selftest.sh` (user process only, nothing installed): it
decodes test frames on the GPU and compares every output path with a CPU reference.
