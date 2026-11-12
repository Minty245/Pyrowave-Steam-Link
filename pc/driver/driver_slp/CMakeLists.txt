cmake_minimum_required(VERSION 3.22)
project(driver_slp CXX)

# ---------------------------------------------------------------------------
# Optional: full OpenVR SDK headers (not required; we bundle a subset).
# If set, the full openvr_driver.h is used instead.
#   cmake -B build -DOPENVR_SDK_INCLUDE_DIR=C:/repos/openvr/headers
# ---------------------------------------------------------------------------
set(OPENVR_SDK_INCLUDE_DIR "" CACHE PATH
    "OpenVR SDK include directory (optional; falls back to bundled subset)")

# ---------------------------------------------------------------------------
# Source
# ---------------------------------------------------------------------------

add_library(driver_slp SHARED driver_slp.cpp)

set_target_properties(driver_slp PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME           driver_slp
    PREFIX                ""
    SUFFIX                ".dll"
)

# ---------------------------------------------------------------------------
# Includes
# ---------------------------------------------------------------------------

target_include_directories(driver_slp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}  # openvr_driver_subset.h
)
if(OPENVR_SDK_INCLUDE_DIR)
    # Full SDK takes precedence via the __has_include() check in the source.
    target_include_directories(driver_slp BEFORE PRIVATE ${OPENVR_SDK_INCLUDE_DIR})
endif()

# ---------------------------------------------------------------------------
# Compile flags
# ---------------------------------------------------------------------------

target_compile_options(driver_slp PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /O2 /GS>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -O2 -fstack-protector-strong>
)

# ---------------------------------------------------------------------------
# Link
# ---------------------------------------------------------------------------

target_link_libraries(driver_slp PRIVATE
    kernel32
    pathcch   # PathCchRemoveFileSpec (Windows 8+)
)
if(MINGW)
    # no MinGW runtime DLLs next to vrserver: link libstdc++ / libgcc / winpthread statically
    target_link_options(driver_slp PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# Explicit export of HmdDriverFactory (MSVC doesn't need a .def for a single export,
# but it makes the intent clear and avoids decorated-name issues with MinGW).
target_sources(driver_slp PRIVATE driver_slp.def)

# ---------------------------------------------------------------------------
# Post-build: copy DLL into the driver's expected bin\win64\ layout so the
# install script can point vrpathreg at this build tree directly.
# ---------------------------------------------------------------------------

set(DRIVER_BIN_DIR "${CMAKE_BINARY_DIR}/driver_slp/bin/win64")
add_custom_command(TARGET driver_slp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DRIVER_BIN_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:driver_slp>"
        "${DRIVER_BIN_DIR}/driver_slp.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/driver_slp.vrdrivermanifest"
        "${CMAKE_BINARY_DIR}/driver_slp/driver_slp.vrdrivermanifest"
    COMMENT "Staging driver_slp layout"
)
