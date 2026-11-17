# Steam Link Pyrowave

Streams SteamVR to the **Steam Frame** with the [Pyrowave](https://github.com/Themaister/pyrowave) codec instead of
HEVC, inside Steam Link itself. Steam Link keeps doing everything else: its foveated view packing, eye tracking,
networking, pacing, menus and bitrate setting. Only the video codec changes.

Pyrowave is an intra-only wavelet codec that runs on the GPU. Every frame stands on its own, so a lost frame is fixed by
the very next one: no smearing or waiting for a keyframe after a hiccup. At high bitrates it holds fine detail and text
very well. It is also fast: in a demanding game we measured about **13 ms less latency** than HEVC (see
[Latency](#latency)).

**What you get**

- Pyrowave encoding on the PC GPU and decoding on the Frame's GPU, chosen per stream: switch Pyrowave off and Steam
  Link streams HEVC exactly as before.
- Foveation tuned for the codec: detail you cannot see away from your gaze is removed before encoding, and the bits go
  to where you look (gaze from the Frame's eye tracking).
- Headset filters on the Frame: a contrast-preserving blur and a stability filter for the periphery, and sharpening
  for the centre.
- Colour handled the way Steam Link handles it for HEVC: a sharper colour downsample and luma adjustment, so
  saturated edges and coloured text keep their brightness (also in the periphery's colour blur).
- Optional **full colour**: colour at full resolution (4:4:4) and 10-bit output into the Frame's compositor.
- **Pyrowave Link**, a settings app on the PC and in the SteamVR dashboard. Changes apply live.

- Lower latency in heavy games: Pyrowave's encode and decode take about the same time whatever the picture, where
  HEVC's encode grew to around 16 ms in our heaviest test.

**Costs**

- The Frame decodes on its GPU, which uses more battery than the hardware HEVC decoder (roughly 15-17 W against
  9-10 W in our tests at 144 Hz).
- An intra-only codec needs more bitrate than HEVC for the same quality. A fast, clean wireless link (6 GHz, little
  interference) and a high Steam Link bandwidth target work best.

## Latency

Measured from Steam Link's own debug graphs (SteamVR Remote Detailed Debug), Elite Dangerous at about 65 fps, Steam
Frame at 144 Hz, one session per codec on the same PC and network. Times are averages per frame, in ms.

| | Pyrowave | HEVC (Steam Link) |
|---|---|---|
| Encode (PC) | 2.0 | 15.8 |
| Send and receive | 10.6 | 5.9 |
| Decode (Frame) | 4.2 | 7.3 |
| **Total, start of frame to display** | **40.8** | **53.5** |
| Typical range (10th to 90th percentile) | 36 to 46 | 48 to 59 |

Pyrowave's encode and decode cost about the same however complex the picture is. Its higher bitrate costs a few
milliseconds more on the network, but the encode saving is much larger. The gap is biggest in demanding games like
this one; in games that are light to render and easy to encode, HEVC's encode is faster and the difference is
smaller. Results depend on the PC, the game, the network and the bitrate.

## Requirements

- Windows 10 or 11 PC with an **NVIDIA** GPU (Steam Link uses NVENC there; Pyrowave takes over its encoder calls).
- SteamVR with Steam Link, and a Steam Frame.

## Install

Download both packages from the release page.

**PC:** extract `SteamLinkPyrowave-PC-<version>.zip`, close SteamVR and double-click `Install.cmd`. It installs to
`%LOCALAPPDATA%\Programs\SteamLinkPyrowave` (no administrator rights), registers the SteamVR driver, adds
**Pyrowave Link** to the Start menu and an uninstall entry to *Settings > Apps*.

**Steam Frame**, either way:

- *On the headset:* in Desktop Mode, download `SteamLinkPyrowave-Frame-<version>.tar.gz`, extract it, open a
  terminal (Konsole) in that folder and run `./install.sh`.
- *From the PC over SSH:* enable SSH on the Frame once (Desktop Mode, Konsole: `passwd` to set a password, then
  `sudo systemctl enable --now sshd`), then double-click `Install-Frame.cmd` in the PC package and enter the Frame's
  address. You type the password when asked; it is not stored.

Both ways offer to reboot the Frame at the end: say yes. Restarting only SteamVR can leave some of its processes on
the previous version, and the Frame may then not shut down cleanly the first time. Everything on the Frame lives in
your home folder (`~/.local/lib/slp` and one Vulkan layer manifest); nothing in the system or in SteamVR's files is
changed.

**Full colour** (optional): run `./install.sh --full-colour` on the Frame (or answer "y" in `Install-Frame.cmd`),
reboot the Frame, then switch *Full colour* on in Pyrowave Link. Only switch it on in the app when the Frame has
it enabled: otherwise the headset shows no picture.

## Use

Start SteamVR and connect with Steam Link as usual. The stream is Pyrowave whenever *Pyrowave streaming* is on in
Pyrowave Link (the default); switching it takes effect when the stream restarts.

- **Bitrate:** Pyrowave follows the bandwidth target in SteamVR's Steam Link settings. Raise that target for quality;
  keep the multiplier near 1.0 so packet pacing stays clean.
- **Pyrowave Link** opens from the Start menu and from the SteamVR dashboard. The defaults are a tested tune; every
  setting has a *Default* button, and *Mine* takes you back to your value, for quick A/B comparisons.

## Uninstall

- PC: *Settings > Apps > Steam Link Pyrowave*, or `Uninstall.cmd` in the install folder. Your settings stay in
  `%LOCALAPPDATA%\SteamLinkPyrowave` unless you run `Uninstall.ps1 -RemoveSettings`.
- Frame: `~/.local/lib/slp/uninstall.sh`, then reboot the Frame. To switch the headset part off without removing
  it: `touch ~/.local/lib/slp/DISABLED`.

## Troubleshooting

See [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md). Logs: PC `%LOCALAPPDATA%\SteamLinkPyrowave\logs`,
Frame `/tmp/slp_layer/`.

## How it works

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). In short:

- **PC** (`pc/`): a small SteamVR driver loads the backend into SteamVR's server process. The backend answers Steam
  Link's NVENC calls itself: it prefilters Steam Link's foveated atlas, encodes it with Pyrowave on a high-priority
  Vulkan compute queue and hands the bytes back in place of HEVC. Steam Link sends them as usual.
- **Frame** (`frame/`): a Vulkan layer in Steam Link's headset client stands in for the hardware video decoder when a
  Pyrowave stream arrives, decodes on the GPU into the same surfaces, and runs the headset filters. With full colour,
  the same layer in the compositor swaps those surfaces for 10-bit full-colour images.
- **App** (`app/`): Pyrowave Link (RmlUi), writes `settings.json`, which the backend reads live and forwards headset
  settings inside the stream.

## Building

From Linux, cross-compiling for Windows and the Frame: see [docs/BUILDING.md](docs/BUILDING.md) and
`scripts/build-release.sh`. Pyrowave itself comes prebuilt in `prebuilt/` from the
[Steam Link Pyrowave fork](https://github.com/Minty245/pyrowave/tree/steam-link-pyrowave) (see
[prebuilt/README.md](prebuilt/README.md)).

## AI disclosure

This project was built with substantial help from AI: much of the code, the tools and the documentation were written
with Anthropic's Claude, directed by the author, who tested every build on the hardware and reviewed what went in.
Report problems through the issue tracker like any other bug.

## Credits and licence

Pyrowave and Granite are by Hans-Kristian Arntzen (Themaister). This project is MIT licensed (see `LICENSE`);
third-party components are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Not affiliated with Valve or
NVIDIA.
