set(srcs "app_main.cpp" "usb_radio.cpp" "net.cpp" "web.cpp")
if(CONFIG_IC7100_DISPLAY)
    # On-device touch interface (LVGL) for the Waveshare ESP32-P4-WIFI6-Touch-LCD-7B.
    list(APPEND srcs "ui.cpp" "display.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES core esp_app_format esp_wifi esp_netif esp_event nvs_flash esp_http_server spiffs esp_timer lwip
    EMBED_TXTFILES "web/index.html")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-format -Wno-missing-field-initializers)
