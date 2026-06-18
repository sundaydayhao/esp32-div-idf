file(GLOB IRREMOTE_SRCS
    "${ARDUINO_LIB_DIR}/IRremoteESP8266/src/*.cpp"
)

idf_component_register(
    SRCS ${IRREMOTE_SRCS}
    INCLUDE_DIRS
        "${ARDUINO_LIB_DIR}/IRremoteESP8266/src"
    REQUIRES
        arduino
)
