# Prebuilt Pyrowave libraries

Built from the Steam Link Pyrowave fork of Pyrowave,
[Minty245/pyrowave](https://github.com/Minty245/pyrowave/tree/steam-link-pyrowave), branch `steam-link-pyrowave`:
upstream [Themaister/pyrowave](https://github.com/Themaister/pyrowave) `7fe0e26` plus two commits, `dd17a40` (decoder
output dither, foveated rate-control weights, per-tile CSF) and `b46771b` (Steam Link style 4:2:0 conversion: sharp
chroma downsample and luma adjustment).

| File | Target | Build |
|---|---|---|
| `win64/libpyrowave-shared-1.dll`, `win64/libpyrowave-shared.dll.a` | PC (SteamVR backend) | MinGW-w64 x86_64, Release |
| `aarch64/libpyrowave-shared.so.1` | Steam Frame (decoder) | aarch64-linux-gnu, Release, `-static-libstdc++ -static-libgcc` |

Shaders: the fork's own shaders build with `shaders/slp_shaders.sh`; changed upstream shaders are spliced into the
generated `shaders/slangmosh.hpp` with `shaders/slangmosh_patch.py` (no slangmosh tool needed).

Rebuild (from the fork checkout: `git clone -b steam-link-pyrowave https://github.com/Minty245/pyrowave`, then
`./checkout_granite.sh`):

```sh
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/x86_64-w64-mingw32.cmake \
      -DGRANITE_SHIPPING=ON -DGRANITE_PLATFORM=null -DGRANITE_RENDERER=OFF -DGRANITE_AUDIO=OFF -DGRANITE_FFMPEG=OFF
ninja -C build-win pyrowave-shared
cmake -S . -B build-a64 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/aarch64-linux-gnu.cmake \
      -DGRANITE_SHIPPING=ON -DGRANITE_PLATFORM=null -DGRANITE_RENDERER=OFF -DGRANITE_AUDIO=OFF -DGRANITE_FFMPEG=OFF \
      -DGRANITE_POSITION_INDEPENDENT=ON -DCMAKE_SHARED_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
ninja -C build-a64 pyrowave-shared
```
