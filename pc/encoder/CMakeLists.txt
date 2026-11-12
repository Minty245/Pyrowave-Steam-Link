cmake_minimum_required(VERSION 3.22)
project(SteamFramePyrowaveBackend CXX)

# ---------------------------------------------------------------------------
# This target builds as a 64-bit Windows DLL (x86_64-w64-mingw32 on Linux, or
# MSVC on Windows).
#
# From a Linux cross-compile environment:
#   cmake -B build \
#         -DCMAKE_TOOLCHAIN_FILE=../../cmake/x86_64-w64-mingw32.cmake \
#         -DPYROWAVE_DIR=/path/to/pyrowave
#
# Native Windows (MSVC or MinGW):
#   cmake -B build -DPYROWAVE_DIR=/path/to/pyrowave
# ---------------------------------------------------------------------------

set(PYROWAVE_DIR "" CACHE PATH
    "Root of the Pyrowave source tree (must contain pyrowave.h).  Also used to find\
 the import library unless PYROWAVE_LIB is set explicitly.")
if(NOT PYROWAVE_DIR)
    message(FATAL_ERROR "Set -DPYROWAVE_DIR=/path/to/pyrowave (source root with pyrowave.h)")
endif()

# PYROWAVE_LIB: explicit path to the import library.
# Defaults: MSVC → <dir>/pyrowave.lib,  MinGW → <dir>/libpyrowave-shared.dll.a
set(PYROWAVE_LIB "" CACHE FILEPATH "Pyrowave import library (auto-detected if empty)")

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

add_library(SteamFramePyrowaveBackend SHARED
    SteamFramePyrowaveBackend.cpp
    slp_nvenc_hook.cpp
    slp_shadow.cpp
    slp_periph.cpp
    slp_gaze.cpp
    slp_settings.cpp
    slp_status.cpp
)

set_target_properties(SteamFramePyrowaveBackend PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME           SteamFramePyrowaveBackend
    PREFIX                ""
    SUFFIX                ".dll"
)

# ---------------------------------------------------------------------------
# Vulkan headers (required by pyrowave.h)
# ---------------------------------------------------------------------------

# Optional: point at a Vulkan headers tree (e.g. Pyrowave's Granite submodule):
#   -DVULKAN_INCLUDE_DIR=/path/to/Granite/third_party/khronos/vulkan-headers/include
# Falls back to find_package(Vulkan) or the system include path.
set(VULKAN_INCLUDE_DIR "" CACHE PATH "Vulkan headers directory (optional; falls back to find_package)")
if(NOT VULKAN_INCLUDE_DIR)
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        set(VULKAN_INCLUDE_DIR "${Vulkan_INCLUDE_DIRS}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Include paths
# ---------------------------------------------------------------------------

target_include_directories(SteamFramePyrowaveBackend PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/nvenc   # official nvEncodeAPI.h (NVENC 11.1)
    ${PYROWAVE_DIR}              # pyrowave.h
)
if(VULKAN_INCLUDE_DIR)
    target_include_directories(SteamFramePyrowaveBackend PRIVATE ${VULKAN_INCLUDE_DIR})
endif()

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------

if(MSVC)
    target_compile_options(SteamFramePyrowaveBackend PRIVATE
        /W4
        /WX-        # warnings not fatal (some NVENC fields intentionally unused)
        /O2
        /GS         # stack security cookies
    )
else()
    # MinGW / Clang on Windows.
    target_compile_options(SteamFramePyrowaveBackend PRIVATE
        -Wall
        -Wextra
        -Wno-unused-parameter
        -Wno-missing-field-initializers   # Vulkan / D3D structs are zero-initialised on purpose
        -Wno-cast-function-type           # GetProcAddress / NVENC function tables
        -O2
        -fstack-protector-strong
    )
endif()

# ---------------------------------------------------------------------------
# Link
# ---------------------------------------------------------------------------

# Pyrowave shared library — resolve the import library path.
if(PYROWAVE_LIB)
    set(_pyro_lib "${PYROWAVE_LIB}")
elseif(MSVC)
    set(_pyro_lib "${PYROWAVE_DIR}/pyrowave.lib")
else()
    # MinGW cross or native: pyrowave-shared builds as libpyrowave-shared.dll.a
    set(_pyro_lib "${PYROWAVE_DIR}/libpyrowave-shared.dll.a")
endif()
if(NOT EXISTS "${_pyro_lib}")
    message(FATAL_ERROR "Pyrowave import library not found: ${_pyro_lib}\n"
        "Pass -DPYROWAVE_LIB=/path/to/import.lib explicitly.")
endif()
target_link_libraries(SteamFramePyrowaveBackend PRIVATE "${_pyro_lib}")

# Windows system libraries needed by our code.
target_link_libraries(SteamFramePyrowaveBackend PRIVATE
    kernel32
    psapi
    shell32
)

# ---------------------------------------------------------------------------
# DEF file: control exports precisely (SFAO checks for exact names).
# ---------------------------------------------------------------------------

# On MSVC supply the .def at compile time; on MinGW (native or cross) pass it
# via the linker (both MINGW==TRUE on a native Windows MinGW build AND when
# cross-compiling with x86_64-w64-mingw32-g++ where MINGW may be unset).
if(MSVC)
    target_sources(SteamFramePyrowaveBackend PRIVATE SteamFramePyrowaveBackend.def)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # MinGW (native or cross): use the .def as a linker script.
    target_sources(SteamFramePyrowaveBackend PRIVATE SteamFramePyrowaveBackend.def)
endif()

# Statically link the MinGW runtime so the DLL only depends on system DLLs +
# libpyrowave-shared-1.dll.  vrserver.exe's LoadLibrary search path does not
# include our folder, so any extra runtime DLLs would fail to resolve.
if(NOT MSVC)
    target_link_options(SteamFramePyrowaveBackend PRIVATE
        -static -static-libgcc -static-libstdc++)
endif()
