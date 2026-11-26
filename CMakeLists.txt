cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Board: 7b = Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (touch screen, 32 MB flash; the default),
#        nano = Waveshare ESP32-P4-NANO (no screen, 16 MB flash; built with release.ps1 -Board nano).
# The board is part of the firmware's name (rigdeck / rigdeck-nano), so a unit only installs firmware for its own
# board, and each board finds its releases in its own folder on the RigDeck site.
if(NOT DEFINED RIGDECK_BOARD)
    set(RIGDECK_BOARD "7b")
endif()
if(RIGDECK_BOARD STREQUAL "nano")
    idf_build_set_property(COMPILE_DEFINITIONS "RIGDECK_SITE_DIR=\"nano/\"" APPEND)
    # The firmware version comes from version.txt (ESP-IDF reads it automatically).
    project(rigdeck-nano)
else()
    idf_build_set_property(COMPILE_DEFINITIONS "RIGDECK_SITE_DIR=\"\"" APPEND)
    project(rigdeck)
endif()
