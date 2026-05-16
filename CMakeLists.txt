# Cross-platform build for Invisible AI Overlay.
#
# The Windows build is also produced by build_release.ps1 (faster for local
# iteration). CMake exists so the macOS port can land in the same repo without
# carrying two build systems forever.
#
# Quick start (Windows, MSYS2 mingw-w64):
#   cmake -B build -G "MinGW Makefiles"
#   cmake --build build --config Release
#   .\build\overlay.exe
#
# Quick start (macOS — once macos/ sources exist):
#   cmake -B build
#   cmake --build build
#   ./build/InvisibleAIOverlay.app/Contents/MacOS/overlay

cmake_minimum_required(VERSION 3.16)
project(InvisibleAIOverlay
    VERSION 2.4.3
    LANGUAGES CXX
    DESCRIPTION "Invisible AI Overlay — live interview & study copilot"
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Cross-platform sources (compile on every target)
# ---------------------------------------------------------------------------
set(COMMON_SOURCES
    ConfigLoader.cpp
    LLMClient.cpp
    Logger.cpp
    Updater.cpp
)
set(COMMON_HEADERS
    ConfigLoader.h
    LLMClient.h
    Logger.h
    Updater.h
    Parsers.h
)

# ---------------------------------------------------------------------------
# Platform-specific sources
# ---------------------------------------------------------------------------
if(WIN32)
    set(PLATFORM_SOURCES
        main.cpp
        OverlayWindow.cpp
        Overlay_Rendering.cpp
        ConfigDialog.cpp
        AudioCapture.cpp
    )

    # Compile the Win32 resource (icon + manifest + version info)
    enable_language(RC)
    list(APPEND PLATFORM_SOURCES app.rc)

    add_executable(overlay WIN32 ${COMMON_SOURCES} ${COMMON_HEADERS} ${PLATFORM_SOURCES})

    target_compile_definitions(overlay PRIVATE UNICODE _UNICODE)
    target_link_libraries(overlay PRIVATE
        winhttp comctl32 gdiplus crypt32 ole32 shell32 dwmapi
    )

    if(MINGW)
        # Match the Defender-friendly static link the PowerShell build uses
        target_link_options(overlay PRIVATE -static -mwindows)
    endif()

elseif(APPLE)
    # macOS sources land here once the port is underway. Until then this
    # branch intentionally fails so nobody mistakes an unfinished port for
    # a working build.
    message(FATAL_ERROR
        "macOS build target not yet implemented. "
        "The port is planned; see ROADMAP. "
        "For now build on Windows.")

else()
    message(FATAL_ERROR "Unsupported platform; Linux build not planned.")
endif()

# ---------------------------------------------------------------------------
# Standalone test runner (parsers only — pure C++, no platform deps)
# ---------------------------------------------------------------------------
add_executable(tests tests.cpp)
target_compile_definitions(tests PRIVATE UNICODE _UNICODE)
set_target_properties(tests PROPERTIES OUTPUT_NAME "tests")

# ---------------------------------------------------------------------------
# Convenience: package project_app/ same as build_release.ps1 does, on demand
# ---------------------------------------------------------------------------
if(WIN32)
    add_custom_target(package
        DEPENDS overlay
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/project_app"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:overlay>" "${CMAKE_SOURCE_DIR}/project_app/overlay.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/models_list.txt" "${CMAKE_SOURCE_DIR}/project_app/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/README.md" "${CMAKE_SOURCE_DIR}/project_app/"
        COMMENT "Staging project_app/ for distribution"
    )
endif()
