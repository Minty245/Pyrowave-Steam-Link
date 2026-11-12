# Pyrowave Link settings app.
#   Windows (MinGW or MSVC): PyrowaveLink.exe - desktop window, and the SteamVR dashboard overlay with --overlay.
#   Linux: slp_preview - offscreen preview harness for screenshots/tests (not shipped).
cmake_minimum_required(VERSION 3.16)
project(PyrowaveLink CXX C)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(RmlUi REQUIRED)

set(BACKENDS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rmlui_backends)
set(COMMON_SRC src/settings_model.cpp src/ui.cpp ${BACKENDS}/RmlUi_Renderer_GL3.cpp)

if(WIN32)
    set(OPENVR_DIR "" CACHE PATH "openvr checkout (headers/ and lib/win64/)")
    add_executable(PyrowaveLink WIN32 src/main_win32.cpp ${COMMON_SRC} ${BACKENDS}/RmlUi_Platform_Win32.cpp app.rc)
    target_include_directories(PyrowaveLink PRIVATE ${BACKENDS} src ${OPENVR_DIR}/headers)
    target_compile_definitions(PyrowaveLink PRIVATE NOMINMAX)
    target_link_libraries(PyrowaveLink PRIVATE RmlUi::RmlUi ${OPENVR_DIR}/lib/win64/openvr_api.lib opengl32 shell32 imm32 dwmapi)
    if(MINGW)
        target_link_options(PyrowaveLink PRIVATE -municode -static -static-libgcc -static-libstdc++)
    endif()
else()
    find_package(glfw3 REQUIRED)
    find_package(OpenGL REQUIRED)
    add_executable(slp_preview src/main_preview.cpp ${COMMON_SRC} ${BACKENDS}/RmlUi_Platform_GLFW.cpp)
    target_include_directories(slp_preview PRIVATE ${BACKENDS} src)
    target_link_libraries(slp_preview PRIVATE RmlUi::RmlUi RmlUi::Debugger glfw OpenGL::GL ${CMAKE_DL_LIBS})
endif()
