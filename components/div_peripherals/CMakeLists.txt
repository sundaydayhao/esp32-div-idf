idf_component_register(
    SRCS
        "src/button_input.c"
        "src/pcf8574.c"
        "src/cc1101.c"
        "src/rf24.c"
        "src/pn532.c"
        "src/ook_radio.c"
        "src/ir_driver.c"
    INCLUDE_DIRS "include"
    REQUIRES div_board div_common esp_driver_i2c
)

set_target_properties(${COMPONENT_LIB} PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED YES)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu17 -fstack-usage -Wno-missing-field-initializers)
