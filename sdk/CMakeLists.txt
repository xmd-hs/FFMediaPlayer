cmake_minimum_required(VERSION 3.16)
project(ffplayer_sdk LANGUAGES CXX)
include(GNUInstallDirs)

if (NOT FFMPEG_ROOT AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/../third_party/ffmpeg")
    set(FFMPEG_ROOT "${CMAKE_CURRENT_LIST_DIR}/../third_party/ffmpeg")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig QUIET)
if (PkgConfig_FOUND AND NOT FFMPEG_ROOT)
    pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
        libavformat libavcodec libavutil libswresample libswscale)
endif()

if (NOT TARGET PkgConfig::FFMPEG)
    if (FFMPEG_ROOT)
        # Prefer the checked-in FFmpeg SDK and ignore stale cache entries.
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
    if (NOT FFMPEG_INCLUDE_DIR OR NOT AVFORMAT_LIBRARY OR NOT AVCODEC_LIBRARY OR NOT AVUTIL_LIBRARY OR NOT SWRESAMPLE_LIBRARY OR NOT SWSCALE_LIBRARY)
        message(FATAL_ERROR "FFmpeg not found. Install FFmpeg development files or set FFMPEG_ROOT.")
    endif()
    add_library(ffmpeg_local INTERFACE)
    target_include_directories(ffmpeg_local INTERFACE ${FFMPEG_INCLUDE_DIR})
    target_link_libraries(ffmpeg_local INTERFACE ${AVFORMAT_LIBRARY} ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY} ${SWRESAMPLE_LIBRARY} ${SWSCALE_LIBRARY})
    add_library(PkgConfig::FFMPEG ALIAS ffmpeg_local)
endif()

add_library(ffplayer_sdk STATIC
    core/media_clock.cpp
    core/player_session.cpp
    core/player.cpp
    core/ffmpeg_demuxer.cpp
    core/ffmpeg_decoder.cpp)
target_sources(ffplayer_sdk PRIVATE core/ffmpeg_subtitle_decoder.cpp)
target_sources(ffplayer_sdk PRIVATE platform/audio_sink.cpp platform/video_sink.cpp)

target_include_directories(ffplayer_sdk PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include> $<INSTALL_INTERFACE:include/ffplayer>)
target_include_directories(ffplayer_sdk PRIVATE core)
target_link_libraries(ffplayer_sdk PRIVATE PkgConfig::FFMPEG)
target_compile_features(ffplayer_sdk PUBLIC cxx_std_17)

# The SDK target intentionally has no Qt linkage or include path.
set_target_properties(ffplayer_sdk PROPERTIES POSITION_INDEPENDENT_CODE ON)

install(TARGETS ffplayer_sdk ffmpeg_local EXPORT ffplayer_sdkTargets
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
