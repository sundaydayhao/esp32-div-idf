idf_component_register(
    SRCS
        "src/display.c"
        "src/touch_driver.c"
        "src/touch_input.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES div_board div_common esp_lcd espressif__esp_lcd_touch
    PRIV_REQUIRES espressif__esp_lcd_ili9341 atanisoft__esp_lcd_touch_xpt2046
)

set_target_properties(${COMPONENT_LIB} PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED YES)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu17 -fstack-usage -Wno-missing-field-initializers)
