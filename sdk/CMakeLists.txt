cmake_minimum_required(VERSION 3.16)
project(ffplayer_sdk LANGUAGES CXX)
include(GNUInstallDirs)

if (NOT FFMPEG_ROOT AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/../third_party/ffmpeg")
    set(FFMPEG_ROOT "${CMAKE_CURRENT_LIST_DIR}/../third_party/ffmpeg")
endif()

# Portable core = C++17 + FFmpeg only.
# Platform zero-copy bridges are optional and live under platform/.
option(FFPLAYER_ENABLE_HWACCEL
    "Link optional platform HW decode / zero-copy bridges (VideoToolbox, D3D11, VAAPI)"
    ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig QUIET)
if (PkgConfig_FOUND AND NOT FFMPEG_ROOT)
    pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
        libavformat libavcodec libavutil libswresample libswscale libavfilter)
endif()

set(FFPLAYER_HAS_FFMPEG_LOCAL FALSE)
set(FFPLAYER_FFMPEG_LIBRARIES PkgConfig::FFMPEG)
if (NOT TARGET PkgConfig::FFMPEG)
    if (FFMPEG_ROOT)
        set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include" CACHE PATH "FFmpeg include directory" FORCE)
    endif()
    find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h
        HINTS ${FFMPEG_ROOT} ENV FFMPEG_ROOT
        PATH_SUFFIXES include)
    if (WIN32)
        set(FFMPEG_LIB_SUFFIXES lib/win64 lib)
    elseif(APPLE)
        set(FFMPEG_LIB_SUFFIXES lib/macOS lib)
    else()
        set(FFMPEG_LIB_SUFFIXES lib/linux64 lib)
    endif()
    find_library(AVFORMAT_LIBRARY avformat HINTS ${FFMPEG_ROOT} ENV FFMPEG_ROOT PATH_SUFFIXES ${FFMPEG_LIB_SUFFIXES})
    find_library(AVCODEC_LIBRARY avcodec HINTS ${FFMPEG_ROOT} ENV FFMPEG_ROOT PATH_SUFFIXES ${FFMPEG_LIB_SUFFIXES})
    find_library(AVUTIL_LIBRARY avutil HINTS ${FFMPEG_ROOT} ENV FFMPEG_ROOT PATH_SUFFIXES ${FFMPEG_LIB_SUFFIXES})
    find_library(SWRESAMPLE_LIBRARY swresample HINTS ${FFMPEG_ROOT} ENV FFMPEG_ROOT PATH_SUFFIXES ${FFMPEG_LIB_SUFFIXES})
    find_library(SWSCALE_LIBRARY swscale HINTS ${FFMPEG_ROOT} ENV FFMPEG_ROOT PATH_SUFFIXES ${FFMPEG_LIB_SUFFIXES})
    find_library(AVFILTER_LIBRARY avfilter HINTS ${FFMPEG_ROOT} ENV FFMPEG_ROOT PATH_SUFFIXES ${FFMPEG_LIB_SUFFIXES})
    if (NOT FFMPEG_INCLUDE_DIR OR NOT AVFORMAT_LIBRARY OR NOT AVCODEC_LIBRARY OR NOT AVUTIL_LIBRARY OR NOT SWRESAMPLE_LIBRARY OR NOT SWSCALE_LIBRARY OR NOT AVFILTER_LIBRARY)
        message(FATAL_ERROR "FFmpeg not found. Install FFmpeg development files or set FFMPEG_ROOT.")
    endif()
    set(FFPLAYER_FFMPEG_LIBRARIES
        ${AVFORMAT_LIBRARY} ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY}
        ${SWRESAMPLE_LIBRARY} ${SWSCALE_LIBRARY} ${AVFILTER_LIBRARY})
    set(FFPLAYER_HAS_FFMPEG_LOCAL TRUE)
endif()

# Keep FFmpeg as a named dependency. Exporting raw find_library() values would
# embed this machine's absolute paths in ffplayer_sdkTargets.cmake.
add_library(ffplayer::ffmpeg INTERFACE IMPORTED GLOBAL)
if (TARGET PkgConfig::FFMPEG)
    set_property(TARGET ffplayer::ffmpeg PROPERTY
        INTERFACE_LINK_LIBRARIES PkgConfig::FFMPEG)
else()
    set_property(TARGET ffplayer::ffmpeg PROPERTY
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}")
    set_property(TARGET ffplayer::ffmpeg PROPERTY
        INTERFACE_LINK_LIBRARIES "${FFPLAYER_FFMPEG_LIBRARIES}")
endif()

# ---- Portable engine (no OS graphics APIs) ----
set(FFPLAYER_CORE_SOURCES
    core/media_clock.cpp
    core/player_session_control.cpp
    core/player_session_sync.cpp
    core/player_session_demux.cpp
    core/player_session_video.cpp
    core/player_session_audio.cpp
    core/player_session_subtitle.cpp
    core/player.cpp
    core/ffmpeg_demuxer.cpp
    core/ffmpeg_decoder.cpp
    core/audio_tempo.cpp
    core/frame_pool.cpp
    core/hw_frame_bridge.cpp
    core/ffmpeg_subtitle_decoder.cpp
    platform/audio_sink.cpp
    platform/video_sink.cpp)

# ---- Optional platform HW bridge (exactly one translation unit provides hwBridgeOps) ----
if (FFPLAYER_ENABLE_HWACCEL)
    if (APPLE)
        list(APPEND FFPLAYER_CORE_SOURCES platform/hw_bridge_videotoolbox.cpp)
    elseif (WIN32)
        list(APPEND FFPLAYER_CORE_SOURCES platform/hw_bridge_d3d11.cpp)
    elseif (UNIX)
        list(APPEND FFPLAYER_CORE_SOURCES platform/hw_bridge_vaapi.cpp)
    else()
        list(APPEND FFPLAYER_CORE_SOURCES platform/hw_bridge_none.cpp)
    endif()
else()
    list(APPEND FFPLAYER_CORE_SOURCES platform/hw_bridge_none.cpp)
endif()

add_library(ffplayer_sdk STATIC ${FFPLAYER_CORE_SOURCES})

target_include_directories(ffplayer_sdk PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
    $<INSTALL_INTERFACE:include/ffplayer>)
target_include_directories(ffplayer_sdk PRIVATE core)
if (FFPLAYER_HAS_FFMPEG_LOCAL)
    target_include_directories(ffplayer_sdk PRIVATE ${FFMPEG_INCLUDE_DIR})
endif()
target_link_libraries(ffplayer_sdk PRIVATE ffplayer::ffmpeg)
target_compile_features(ffplayer_sdk PUBLIC cxx_std_17)
if (MSVC)
    target_compile_options(ffplayer_sdk PRIVATE /utf-8)
endif()

if (FFPLAYER_ENABLE_HWACCEL)
    target_compile_definitions(ffplayer_sdk PUBLIC FFPLAYER_ENABLE_HWACCEL=1)
else()
    target_compile_definitions(ffplayer_sdk PUBLIC FFPLAYER_ENABLE_HWACCEL=0)
endif()

# No Qt / window-system linkage in the SDK target.
set_target_properties(ffplayer_sdk PROPERTIES POSITION_INDEPENDENT_CODE ON)

install(TARGETS ffplayer_sdk EXPORT ffplayer_sdkTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ffplayer)
install(EXPORT ffplayer_sdkTargets FILE ffplayer_sdkTargets.cmake
    NAMESPACE ffplayer:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ffplayer_sdk)
include(CMakePackageConfigHelpers)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/ffplayer_sdkConfigVersion.cmake
    VERSION 1.0.0 COMPATIBILITY SameMajorVersion)
configure_package_config_file(${CMAKE_CURRENT_LIST_DIR}/ffplayer_sdkConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ffplayer_sdkConfig.cmake INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ffplayer_sdk)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ffplayer_sdkConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/ffplayer_sdkConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ffplayer_sdk)

if (UNIX AND NOT APPLE)
    target_link_libraries(ffplayer_sdk PRIVATE pthread)
endif()

if (FFPLAYER_ENABLE_HWACCEL)
    if (APPLE)
        target_link_libraries(ffplayer_sdk PRIVATE
            "-framework VideoToolbox"
            "-framework CoreMedia"
            "-framework CoreVideo"
            "-framework CoreFoundation")
    endif()
    if (WIN32)
        target_link_libraries(ffplayer_sdk PRIVATE d3d11)
    endif()
endif()

message(STATUS "ffplayer_sdk: FFPLAYER_ENABLE_HWACCEL=${FFPLAYER_ENABLE_HWACCEL}")
