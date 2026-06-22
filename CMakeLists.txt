cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/third_party/arduino"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/TFT_eSPI"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=missing-field-initializers" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-missing-field-initializers" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=cpp" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-cpp" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=misleading-indentation" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-misleading-indentation" APPEND)
project(esp32_div_idf)
