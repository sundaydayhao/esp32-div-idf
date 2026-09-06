idf_component_register(
    SRCS
        "src/console.c"
        "src/storage_file.c"
        "src/ble_service.c"
        "src/ble_hid.c"
        "src/storage.c"
        "src/network.c"
        "src/nvs_records.c"
        "src/firmware_update.c"
        "src/captive_dns.c"
    INCLUDE_DIRS "include"
    REQUIRES div_board div_common bt fatfs sdmmc esp_driver_sdspi esp_wifi esp_netif esp_event nvs_flash app_update esp_http_server
)

set_target_properties(${COMPONENT_LIB} PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED YES)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu17 -fstack-usage -Wno-missing-field-initializers)
