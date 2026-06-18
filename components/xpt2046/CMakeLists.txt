idf_component_register(
    SRCS
        "${ARDUINO_LIB_DIR}/XPT2046_Touchscreen/XPT2046_Touchscreen.cpp"
    INCLUDE_DIRS
        "${ARDUINO_LIB_DIR}/XPT2046_Touchscreen"
    REQUIRES
        arduino
)
