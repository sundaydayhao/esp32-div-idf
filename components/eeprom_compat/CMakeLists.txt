idf_component_register(
    SRCS
        "EEPROM.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash
)
