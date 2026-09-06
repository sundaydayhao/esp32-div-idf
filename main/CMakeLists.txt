idf_component_register(
    SRCS
        "features/terminal.c"
        "features/settings_page.c"
        "features/file_manager.c"
        "features/touch_calibration.c"
        "app/app.c"
        "app/feature_text.c"
        "app/settings_store.c"
        "features/bluetooth.c"
        "features/ducky.c"
        "features/gps.c"
        "features/ir.c"
        "features/rfid.c"
        "features/subghz.c"
        "features/wifi.c"
        "main.c"
        "ui/keyboard_ui.c"
        "ui/theme.c"
        "ui/touch_ui.c"
        "ui/ui_self_test.c"
        "ui/ui_utils.c"
    PRIV_INCLUDE_DIRS "app" "features" "ui" "ui/assets"
    PRIV_REQUIRES
        div_common div_board div_peripherals div_display div_services div_gnss div_signal
        bt cjson nvs_flash esp_event esp_system esp_timer esp_wifi esp_netif
        esp_http_client esp_http_server esp_driver_gpio esp_adc esp_lcd
        esp_driver_rmt esp_driver_i2c mdns bootloader_support app_update
        esp_driver_ledc esp_driver_spi esp_driver_sdspi esp_driver_tsens
        esp_driver_uart esp-tls fatfs sdmmc
)

set_target_properties(${COMPONENT_LIB} PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED YES)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu17 -fstack-usage -Wno-missing-field-initializers)
