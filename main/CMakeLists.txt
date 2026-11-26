set(srcs "app_main.cpp" "usb_radio.cpp" "net.cpp" "web.cpp" "peers_net.cpp" "kiss_net.cpp" "gps_module.cpp" "ota.cpp" "memtrace.c" "logo_font.c" "lv_mem_psram.c")
if(IDF_TARGET STREQUAL "esp32p4")
    # Wi-Fi chip (ESP32-C6 co-processor) version and firmware update over ESP-Hosted
    # (a dependency in idf_component.yml, so its headers are already available here).
    list(APPEND srcs "wifi_chip.cpp")
    # microSD card for the log (the Waveshare BSP mounts it)
    list(APPEND srcs "sdcard.c" "sdlog.cpp" "mapfile.cpp" "updcheck.cpp" "k0dog.cpp")
endif()
if(CONFIG_IC7100_DISPLAY)
    # On-device touch interface (LVGL) for the Waveshare ESP32-P4-WIFI6-Touch-LCD-7B.
    list(APPEND srcs "ui.cpp" "display.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES core fatfs sdmmc esp_driver_sdmmc esp_app_format esp_wifi esp_netif esp_event nvs_flash esp_http_server spiffs esp_timer lwip esp_driver_uart esp_driver_gpio esp_partition app_update mbedtls esp_http_client esp-tls
    EMBED_FILES "web/icon.png")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-format -Wno-missing-field-initializers)

# The web pages are kept in flash gzip-compressed (index.html 137 kB -> about 49 kB) and sent with
# Content-Encoding: gzip; every browser unpacks them. They are compressed at build time from main/web.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(python PYTHON)
    foreach(page index multi)
        set(gz "${CMAKE_CURRENT_BINARY_DIR}/${page}.html.gz")
        add_custom_command(OUTPUT "${gz}"
            COMMAND "${python}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gzip_file.py"
                    "${CMAKE_CURRENT_SOURCE_DIR}/web/${page}.html" "${gz}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web/${page}.html" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gzip_file.py"
            VERBATIM)
        add_custom_target(${page}_html_gz DEPENDS "${gz}")
        add_dependencies(${COMPONENT_LIB} ${page}_html_gz)
        target_add_binary_data(${COMPONENT_LIB} "${gz}" BINARY)
    endforeach()
endif()
