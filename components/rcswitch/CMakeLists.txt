idf_component_register(
    SRCS
        "${ARDUINO_LIB_DIR}/rc-switch/RCSwitch.cpp"
    INCLUDE_DIRS
        "${ARDUINO_LIB_DIR}/rc-switch"
    REQUIRES
        arduino
)
