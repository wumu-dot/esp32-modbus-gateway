cmake_minimum_required(VERSION 3.22)

# Exclude old component freemodbus which exists in old versions
set(EXCLUDE_COMPONENTS freemodbus)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Minimal build
set(COMPONENTS main esp_psram)

project(esp32_modbus_gateway)