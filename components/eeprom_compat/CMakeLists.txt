idf_component_register(
    SRCS
        "src/EEPROM.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        nvs_flash
)
