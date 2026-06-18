idf_component_register(
    SRCS
        "${ARDUINO_LIB_DIR}/RF24/RF24.cpp"
    INCLUDE_DIRS
        "${ARDUINO_LIB_DIR}/RF24"
    REQUIRES
        arduino
)
