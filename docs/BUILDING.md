# Building

Everything cross-compiles from Linux (tested on Ubuntu 24.04):

- `x86_64-w64-mingw32` gcc/g++ (posix thread model) for the PC parts, `aarch64-linux-gnu` gcc/g++ for the Frame.
- cmake, ninja, zip; `glslangValidator` only to regenerate the Frame shaders (`frame/decoder/shaders.sh`).
- A checkout of the Steam Link Pyrowave fork of Pyrowave, branch `steam-link-pyrowave` of
  [Minty245/pyrowave](https://github.com/Minty245/pyrowave/tree/steam-link-pyrowave) (with Granite:
  `checkout_granite.sh`).
- RmlUi 6.1 + FreeType 2.13 built with the same MinGW toolchain, and an openvr checkout, for Pyrowave Link.

```sh
PYROWAVE_SRC=~/src/pyrowave RMLUI_PREFIX=~/deps/win OPENVR_DIR=~/src/openvr scripts/build-release.sh
```

produces `dist/SteamLinkPyrowave-PC-<version>.zip` and `dist/SteamLinkPyrowave-Frame-<version>.tar.gz`. The parts
can also be built one by one; see the comments at the top of `pc/encoder/CMakeLists.txt`,
`pc/driver/driver_slp/CMakeLists.txt`, `app/CMakeLists.txt`, `frame/decoder/build.sh` and `frame/layer/build.sh`.

## Tests

- `frame/decoder`: `slp_selftest` (on the Frame), and host tests on any Vulkan device (lavapipe): `fx_test.cpp`
  (folded filters vs separate passes, fast pyramid), `pack_test.cpp`, `postfx_test.cpp`, `wideblur_test.cpp`.
- `frame/layer`: `fc_test.c` (full-colour handover protocol and compositor glue), `adopt_test.c` (decoder adoption).
- Pyrowave fork: `slp-ycbcr420-test` (4:2:0 conversion modes against the upstream scaler; `0` as the first argument
  runs the conversion alone, for devices the encoder does not run on).
- `app`: on Linux the app builds as `slp_preview`, an offscreen harness that renders the UI to an image and can click.
