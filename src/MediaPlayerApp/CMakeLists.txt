cmake_minimum_required(VERSION 3.16)
project(ffplayer_app LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Qt5 REQUIRED COMPONENTS Widgets Multimedia)
add_subdirectory(../../sdk sdk-build)
add_executable(ffplayer_app main.cpp player_window.cpp player_window.h
    adapters/qt_sinks.cpp adapters/qt_sinks.h)
target_link_libraries(ffplayer_app PRIVATE ffplayer_sdk Qt5::Widgets Qt5::Multimedia)
target_include_directories(ffplayer_app PRIVATE ../../sdk/include)
