# Firmware for the board's Wi-Fi chip (ESP32-C6), built from the ESP-Hosted copy in this repository
# so it always matches what RigDeck's main chip speaks. Build with build_wifi_chip.bat (Windows) or:
#     idf.py set-target esp32c6 build
# then send build/rigdeck_wifi_chip.bin to the board from the web page (Settings > Wi-Fi chip).
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rigdeck_wifi_chip)
