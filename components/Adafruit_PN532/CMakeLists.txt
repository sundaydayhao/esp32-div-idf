idf_component_register(
    SRCS
        "${ARDUINO_LIB_DIR}/Adafruit_PN532/Adafruit_PN532.cpp"
    INCLUDE_DIRS
        "${ARDUINO_LIB_DIR}/Adafruit_PN532"
    REQUIRES
        arduino Adafruit_BusIO
)
