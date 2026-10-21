# P4DOS firmware for the Guition JC8012P4A1C-I-W-Y (ESP32-P4). ESP-IDF 5.5.x.
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.4.0")
if(NOT SDKCONFIG_DEFAULTS) # build.sh passes its own for chip revision v3.x
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p4dos)
