idf_component_register(
    SRCS
        "${ARDUINO_LIB_DIR}/PCF8574_library/PCF8574.cpp"
    INCLUDE_DIRS
        "${ARDUINO_LIB_DIR}/PCF8574_library"
    REQUIRES
        arduino
)
