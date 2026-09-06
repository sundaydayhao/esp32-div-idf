idf_component_register(
    SRCS
        "src/board_services.c"
        "src/system_utils.c"
        "src/spi_bus.c"
        "src/spi_device.c"
        "src/pulse_io.c"
        "src/uart_port.c"
    INCLUDE_DIRS "include"
    REQUIRES div_common esp_driver_spi esp_driver_gpio esp_driver_rmt esp_driver_uart esp_timer esp_system
    PRIV_REQUIRES esp_adc esp_driver_tsens espressif__led_strip
)

set_target_properties(${COMPONENT_LIB} PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED YES)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu17 -fstack-usage -Wno-missing-field-initializers)
