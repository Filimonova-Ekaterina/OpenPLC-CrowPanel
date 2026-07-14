idf_component_register(
    SRCS "src/ui_generator.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl data_model opcua_client navigation settings
)
