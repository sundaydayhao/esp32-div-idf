idf_component_register(
    SRCS
        "${ARDUINO_LIB_DIR}/SmartRC-CC1101-Driver-Lib/SmartRC_CC1101.cpp"
    INCLUDE_DIRS
        "${ARDUINO_LIB_DIR}/SmartRC-CC1101-Driver-Lib"
    REQUIRES
        arduino
)
