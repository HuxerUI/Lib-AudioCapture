cmake_minimum_required(VERSION 3.20)

# CMake initializes Apple deployment defaults during the first project() call.
# Explicit settings and toolchains retain ownership of their target platform.
if (CMAKE_HOST_APPLE AND (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        AND NOT CMAKE_TOOLCHAIN_FILE AND "$ENV{CMAKE_TOOLCHAIN_FILE}" STREQUAL ""
        AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment target")
endif ()

project(lib_audio_capture VERSION 0.1.0 LANGUAGES NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/HuxerUIProject.cmake")
if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

set(LIBRARY_SOURCE_FILES
        src/audio_capture.cpp
        src/audio_reader.cpp
        src/capture_run.cpp
        src/audio_recording.cpp
)
if (IOS)
    set(audio_capture_backend platform/ios/src/audio_capture_ios.mm)
elseif (APPLE)
    set(audio_capture_backend platform/macos/src/audio_capture_macos.mm)
elseif (ANDROID)
    set(audio_capture_backend platform/android/src/main/cpp/audio_capture_android.cpp)
elseif (WIN32)
    set(audio_capture_backend platform/windows/src/audio_capture_windows.cpp)
elseif (EMSCRIPTEN)
    set(audio_capture_backend platform/web/src/audio_capture_web.cpp)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(audio_capture_backend platform/linux/src/audio_capture_linux.cpp)
endif ()

huxerui_add_library(huxerui_audiocapture
        SOURCES
            ${LIBRARY_SOURCE_FILES} ${audio_capture_backend}
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            huxerui_audiocapture
)

add_library(HuxerUI::AudioCapture ALIAS huxerui_audiocapture)

if (TARGET HuxerUI::Media)
    huxerui_use_library(huxerui_audiocapture PUBLIC TARGET HuxerUI::Media)
elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../Lib-Media/CMakeLists.txt")
    huxerui_use_library(huxerui_audiocapture PUBLIC
            TARGET HuxerUI::Media
            PATH "${CMAKE_CURRENT_SOURCE_DIR}/../Lib-Media"
    )
else ()
    huxerui_use_library(huxerui_audiocapture PUBLIC
            TARGET HuxerUI::Media
            URL "https://github.com/HuxerUI/Lib-Media.git"
            COMMIT "0ead796ca4895ffeb0b2cce2aac49bb931de77fc"
    )
endif ()

if (HUXERUI_LIBRARY_GRAPH_ONLY)
    return()
endif ()

target_include_directories(huxerui_audiocapture PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)
target_include_directories(huxerui_audiocapture PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if (audio_capture_backend)
    target_compile_definitions(huxerui_audiocapture PRIVATE HUXERUI_AUDIO_CAPTURE_AVAILABLE)
endif ()
if (APPLE)
    target_compile_options(huxerui_audiocapture PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_link_libraries(huxerui_audiocapture PRIVATE "-framework AVFoundation" "-framework CoreMedia"
            "-framework AudioToolbox")
elseif (ANDROID)
    target_link_libraries(huxerui_audiocapture PRIVATE dl)
elseif (WIN32)
    target_compile_definitions(huxerui_audiocapture PRIVATE NOMINMAX)
    target_link_libraries(huxerui_audiocapture PRIVATE ole32)
elseif (EMSCRIPTEN)
    set(audio_capture_web_script "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/audio_capture.js")
    set_property(TARGET huxerui_audiocapture APPEND PROPERTY INTERFACE_LINK_DEPENDS "${audio_capture_web_script}")
    target_link_options(huxerui_audiocapture INTERFACE "SHELL:--pre-js \"${audio_capture_web_script}\"" "--bind")
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(ALSA REQUIRED)
    target_link_libraries(huxerui_audiocapture PRIVATE ALSA::ALSA)
endif ()

option(HUXERUI_AUDIOCAPTURE_BUILD_TESTS "Build AudioCapture contract tests" OFF)
if (HUXERUI_AUDIOCAPTURE_BUILD_TESTS)
    if (NOT TARGET HuxerUI::testing)
        find_package(HuxerUI CONFIG REQUIRED COMPONENTS testing)
    endif ()
    enable_testing()
    add_executable(audio_capture_tests tests/audio_capture_tests.cpp)
    target_compile_features(audio_capture_tests PRIVATE cxx_std_20)
    target_include_directories(audio_capture_tests PRIVATE src)
    target_link_libraries(audio_capture_tests PRIVATE HuxerUI::AudioCapture HuxerUI::testing)
    huxerui_enable_codegen(audio_capture_tests)
    add_test(NAME audio_capture_tests COMMAND audio_capture_tests)
    set_tests_properties(audio_capture_tests PROPERTIES TIMEOUT 20)
endif ()
