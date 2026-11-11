cmake_minimum_required(VERSION 3.20)

# CMake initializes Apple deployment defaults during the first project() call.
# Explicit settings and toolchains retain ownership of their target platform.
if (CMAKE_HOST_APPLE AND (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        AND NOT CMAKE_TOOLCHAIN_FILE AND "$ENV{CMAKE_TOOLCHAIN_FILE}" STREQUAL ""
        AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment target")
endif ()

project(example_lib_audio_capture VERSION 0.1.0 LANGUAGES NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/HuxerUIProject.cmake")
if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

file(GLOB_RECURSE APP_SOURCE_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)
if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    if (WIN32)
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/main.cpp")
    elseif (APPLE AND NOT IOS)
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/main.cpp")
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/main.cpp")
    endif ()
endif ()

huxerui_add_app(example_lib_audio_capture
        SOURCES
            ${APP_SOURCE_FILES}
        RESOURCES
            resources
        RESOURCE_NAMESPACE
            app
        ${HUXERUI_APP_RESOURCE_OUTPUT_ARGUMENTS}
        BUNDLE_NAME
            "Voice Studio"
        BUNDLE_IDENTIFIER
            "org.huxerui.lib.audiocapture.preview"
)

huxerui_configure_project_app(example_lib_audio_capture)

huxerui_use_library(example_lib_audio_capture
        TARGET HuxerUI::AudioCapture
        PATH "${CMAKE_CURRENT_SOURCE_DIR}/../.."
)

if (HUXERUI_LIBRARY_GRAPH_ONLY)
    return()
endif ()

include(FetchContent)
if (POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif ()
FetchContent_Declare(voice_studio_stretch
        URL https://codeload.github.com/Signalsmith-Audio/signalsmith-stretch/tar.gz/a670068d9aeb64913331d5cc29337b19a457a7df
        URL_HASH SHA256=948b8382bb13f316d13536cdfe06111789a538e8fee7f7a531ee2aec94de1747
        SOURCE_SUBDIR include
)
FetchContent_Declare(voice_studio_linear
        URL https://codeload.github.com/Signalsmith-Audio/linear/tar.gz/de55e6a50ffcf6f8f43f649692d94691c7025151
        URL_HASH SHA256=cc39693d5c8bc96451b5503819bb82b2d88c1df450183c55595e5d9d4b517012
        SOURCE_SUBDIR include
)
FetchContent_MakeAvailable(voice_studio_stretch voice_studio_linear)

target_sources(example_lib_audio_capture PRIVATE effects/voice_effects.cpp)
target_include_directories(example_lib_audio_capture PRIVATE effects)
target_include_directories(example_lib_audio_capture SYSTEM PRIVATE
        "${voice_studio_stretch_SOURCE_DIR}/include"
        "${voice_studio_linear_SOURCE_DIR}/include"
)
if (NOT MSVC)
    set_source_files_properties(effects/voice_effects.cpp PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O2>")
endif ()
