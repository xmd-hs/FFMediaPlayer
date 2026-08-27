cmake_minimum_required(VERSION 3.16)
project(ffplayer_app LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)

find_package(Qt5 REQUIRED COMPONENTS Widgets Multimedia)
add_subdirectory(../../sdk sdk-build)

set(FFPLAYER_APP_SOURCES
    main.cpp
    player_window.cpp
    player_window.h
    adapters/qt_sinks.cpp
    adapters/qt_sinks.h)

if (APPLE)
    enable_language(OBJCXX)
    list(APPEND FFPLAYER_APP_SOURCES
        adapters/metal_video_view.h
        adapters/metal_video_view.mm)
endif()

if (WIN32)
    list(APPEND FFPLAYER_APP_SOURCES
        adapters/d3d11_video_view.h
        adapters/d3d11_video_view.cpp)
endif()

add_executable(ffplayer_app ${FFPLAYER_APP_SOURCES})
target_link_libraries(ffplayer_app PRIVATE ffplayer_sdk Qt5::Widgets Qt5::Multimedia)
target_include_directories(ffplayer_app PRIVATE ../../sdk/include adapters)
if (MSVC)
    target_compile_options(ffplayer_app PRIVATE /utf-8)
endif()

if (APPLE)
    target_link_libraries(ffplayer_app PRIVATE
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
        "-framework CoreVideo"
        "-framework AppKit")
    set_source_files_properties(adapters/metal_video_view.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
endif()

if (WIN32)
    target_link_libraries(ffplayer_app PRIVATE d3d11 dxgi d3dcompiler)

    get_filename_component(FFMPEG_RUNTIME_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../../third_party/ffmpeg" ABSOLUTE)
    set(FFMPEG_RUNTIME_DLLS
        avcodec-61.dll
        avdevice-61.dll
        avfilter-10.dll
        avformat-61.dll
        avutil-59.dll
        postproc-58.dll
        swresample-5.dll
        swscale-8.dll)
    foreach(FFMPEG_RUNTIME_DLL IN LISTS FFMPEG_RUNTIME_DLLS)
        add_custom_command(TARGET ffplayer_app POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FFMPEG_RUNTIME_ROOT}/bin/win64/$<IF:$<CONFIG:Debug>,debug,release>/${FFMPEG_RUNTIME_DLL}"
                "$<TARGET_FILE_DIR:ffplayer_app>/${FFMPEG_RUNTIME_DLL}"
            VERBATIM)
    endforeach()

    set(UCRT_PRIVATE_DLL
        "$ENV{SystemDrive}/Program Files (x86)/Windows Kits/10/Redist/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/ucrt/DLLs/x64/api-ms-win-crt-private-l1-1-0.dll")
    if (EXISTS "${UCRT_PRIVATE_DLL}")
        add_custom_command(TARGET ffplayer_app POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${UCRT_PRIVATE_DLL}"
                "$<TARGET_FILE_DIR:ffplayer_app>/api-ms-win-crt-private-l1-1-0.dll"
            VERBATIM)
    else()
        message(FATAL_ERROR
            "Private UCRT runtime required by bundled FFmpeg was not found: ${UCRT_PRIVATE_DLL}")
    endif()
endif()
