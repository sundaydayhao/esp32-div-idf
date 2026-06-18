cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components/esp32_div"
    "${CMAKE_CURRENT_LIST_DIR}/components/pcf8574"
    "${CMAKE_CURRENT_LIST_DIR}/components/xpt2046"
    "${CMAKE_CURRENT_LIST_DIR}/components/thirdparty_compat"
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
