# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Set the project name
set(PROJECT_NAME "bluetooth_music_player_cyd")
set(EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/CYD-BT")

# Include ESP-IDF cmake functions
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(${PROJECT_NAME})