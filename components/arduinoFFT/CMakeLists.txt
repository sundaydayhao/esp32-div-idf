idf_component_register(
    SRCS
        "${ARDUINO_LIB_DIR}/arduinoFFT/src/arduinoFFT.cpp"
    INCLUDE_DIRS
        "${ARDUINO_LIB_DIR}/arduinoFFT/src"
    REQUIRES
        arduino
)
