idf_component_register(
    SRCS
        "src/gnss_parser.c"
        "src/gnss_receiver.c"
    INCLUDE_DIRS "include"
    REQUIRES div_common div_board
)

set_target_properties(${COMPONENT_LIB} PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED YES)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu17 -fstack-usage -Wno-missing-field-initializers)
