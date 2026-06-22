idf_component_register(
    SRCS
        "src/PCF8574.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_driver_i2c
        driver
    PRIV_REQUIRES
        esp_driver_gpio
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PCF8574_I2C_SDA=GPIO_NUM_8
    PCF8574_I2C_SCL=GPIO_NUM_9
    PCF8574_I2C_FREQ_HZ=100000
)
