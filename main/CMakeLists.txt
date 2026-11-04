idf_component_register(
    SRCS "main.c"
         "app_ui.c"
         "app_config.c"
         "wifi_sta.c"
         "ma_client.c"
         "voice_client.c"
         "provisioning.c"
    INCLUDE_DIRS "."
    REQUIRES bsp lvgl esp_wifi esp_netif esp_event nvs_flash esp_http_client json esp_http_server
    PRIV_REQUIRES esp_timer esp_hw_support esp_driver_usb_serial_jtag
)

target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_20.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_16.c"
)

