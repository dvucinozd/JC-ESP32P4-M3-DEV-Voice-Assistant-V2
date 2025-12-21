# ESP32-P4 Voice Assistant for Home Assistant
# Based on mp3_player demo with Voice Assistant capabilities
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    common_components
    )

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "P4-HA-VA-0.1.1")
project(esp32_p4_voice_assistant)
