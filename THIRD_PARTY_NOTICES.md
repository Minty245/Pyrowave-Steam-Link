# Third-party components

| Component | Used in | Licence |
|---|---|---|
| [Pyrowave](https://github.com/Themaister/pyrowave) by Hans-Kristian Arntzen, with the Steam Link Pyrowave fork changes (`prebuilt/`) | PC backend, Frame decoder | MIT |
| [Granite](https://github.com/Themaister/Granite) by Hans-Kristian Arntzen (built into Pyrowave) and its bundled third-party code (volk, Vulkan headers, SPIRV tools, ...) | PC backend, Frame decoder | MIT and the licences in Granite's `third_party/` |
| NVENC API header `nvEncodeAPI.h` (NVIDIA, via FFmpeg nv-codec-headers) | `pc/third_party/nvenc` | MIT-style, see the header |
| OpenVR / OpenVR driver headers and `openvr_api.dll` (Valve) | SteamVR driver, Pyrowave Link | BSD-3-Clause |
| [RmlUi](https://github.com/mikke89/RmlUi) and its GL3 / Win32 / GLFW backends | Pyrowave Link | MIT |
| [FreeType](https://freetype.org) | Pyrowave Link (via RmlUi) | FreeType Licence (FTL) |
| [Inter](https://rsms.me/inter/) and [JetBrains Mono](https://www.jetbrains.com/lp/mono/) fonts | Pyrowave Link | SIL Open Font License 1.1 |

The licence texts ship with the release packages (`app/PyrowaveLink/licenses`, `app/assets/fonts`, `driver/slp`).

Steam, SteamVR, Steam Link and Steam Frame are trademarks of Valve Corporation. This project is not affiliated with or
endorsed by Valve or NVIDIA. It does not modify any Valve files and redistributes none apart from the BSD-licensed
OpenVR runtime (`openvr_api.dll`): on the PC it is a SteamVR driver that loads next to Steam Link's own driver, and on
the Frame it is a Vulkan layer installed in the user's home folder.
