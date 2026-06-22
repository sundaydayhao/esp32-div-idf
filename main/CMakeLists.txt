idf_component_register(
    SRCS
        "main.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp32_div_board
        esp32_div
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
