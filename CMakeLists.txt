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
# Quick start (macOS):
#   cmake -B build
#   cmake --build build
#   open build/overlay.app

cmake_minimum_required(VERSION 3.16)
project(InvisibleAIOverlay
    VERSION 2.5.2
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
    IAudioCapture.h
    IScreenshot.h
    HttpClient.h
    WinCompat.h
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
        Screenshot_Win.cpp
        HttpClient_Win.cpp
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
    enable_language(OBJCXX)

    set(MAC_SOURCES
        macos/main_mac.mm
        macos/MacAudioCapture.mm
        macos/Screenshot_Mac.mm
        macos/MacOverlayWindow.mm
        macos/MacRenderer.mm
        macos/MacConfigDialog.mm
        macos/HttpClient_Mac.mm
    )

    add_executable(overlay MACOSX_BUNDLE
        ${COMMON_SOURCES} ${COMMON_HEADERS} ${MAC_SOURCES})

    set_target_properties(overlay PROPERTIES
        OUTPUT_NAME "overlay"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/macos/Info.plist"
        MACOSX_BUNDLE_BUNDLE_NAME "Invisible AI Overlay"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.zer0skillman.interview-hack"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    )

    # ScreenCaptureKit needs macOS 12.3+; SCScreenshotManager (the only
    # supported screenshot API after CGDisplayCreateImage was removed in
    # macOS 15) is 14+, so 14.0 is the minimum target.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "" FORCE)
    set_target_properties(overlay PROPERTIES
        XCODE_ATTRIBUTE_MACOSX_DEPLOYMENT_TARGET "14.0"
    )

    # ARC for all .mm files. The retain/release rules become automatic, so
    # __bridge_retained / __bridge_transfer casts actually transfer ownership
    # the way the code assumes.
    set_source_files_properties(${MAC_SOURCES} PROPERTIES
        COMPILE_FLAGS "-fobjc-arc")

    # libcurl ships with macOS — find it via pkg-config or the framework path.
    find_package(CURL REQUIRED)
    target_link_libraries(overlay PRIVATE CURL::libcurl)

    target_link_libraries(overlay PRIVATE
        "-framework Cocoa"
        "-framework AppKit"
        "-framework Foundation"
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework ScreenCaptureKit"
        "-framework CoreGraphics"
        "-framework ImageIO"
        "-framework UniformTypeIdentifiers"
        "-framework Carbon"
    )

    target_compile_options(overlay PRIVATE
        -Wno-deprecated-declarations   # Carbon hotkey API is deprecated but still the right answer
    )

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
