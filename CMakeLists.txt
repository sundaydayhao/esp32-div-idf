cmake_minimum_required(VERSION 3.16)

set(ARDUINO_CORE_DIR "$ENV{USERPROFILE}/AppData/Local/Arduino15/packages/esp32/hardware/esp32/3.3.10-cn" CACHE PATH "Arduino ESP32 core root")
set(ARDUINO_LIB_DIR "$ENV{USERPROFILE}/Documents/Arduino/libraries" CACHE PATH "Arduino library root")

set(EXTRA_COMPONENT_DIRS
    "${ARDUINO_CORE_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/components/esp32_div"
    "${CMAKE_CURRENT_LIST_DIR}/components/Adafruit_BusIO"
    "${CMAKE_CURRENT_LIST_DIR}/components/Adafruit_PN532"
    "${CMAKE_CURRENT_LIST_DIR}/components/arduinoFFT"
    "${CMAKE_CURRENT_LIST_DIR}/components/pcf8574"
    "${CMAKE_CURRENT_LIST_DIR}/components/xpt2046"
    "${CMAKE_CURRENT_LIST_DIR}/components/rcswitch"
    "${CMAKE_CURRENT_LIST_DIR}/components/smartrc_cc1101"
    "${CMAKE_CURRENT_LIST_DIR}/components/rf24"
    "${CMAKE_CURRENT_LIST_DIR}/components/irremoteesp8266"
    "${CMAKE_CURRENT_LIST_DIR}/components/nimble_compat"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=missing-field-initializers" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-missing-field-initializers" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=cpp" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-cpp" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=misleading-indentation" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-misleading-indentation" APPEND)
project(esp32_div_idf)
