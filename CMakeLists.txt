# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.10)

# The configuration is to prevent compilation errors on the Windows system.
set(CMAKE_OBJECT_PATH_MAX 512)

set(COMPONENTS "main" "espressif__esp32_p4_function_ev_board")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(open_plc VERSION 0.0.1)
