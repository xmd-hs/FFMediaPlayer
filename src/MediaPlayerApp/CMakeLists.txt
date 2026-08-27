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
endif()
