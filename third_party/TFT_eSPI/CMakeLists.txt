
set(ESP32_DIV_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../ESP32-DIV")

idf_component_register(SRCS "TFT_eSPI.cpp"
                    INCLUDE_DIRS "." "${ESP32_DIV_SRC_DIR}"
                    PRIV_REQUIRES arduino)
