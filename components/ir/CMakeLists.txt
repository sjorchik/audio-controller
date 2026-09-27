idf_component_register(
    SRCS "ir.c" "ir_dev.c"
    INCLUDE_DIRS "include"
    REQUIRES driver settings audio tda7318 system console
)