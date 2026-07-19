idf_component_register(
    SRCS
        "src/ui_generator.c"
        "src/ui_theme.c"
        "src/ui_overview.c"
        "src/ui_equipment.c"
        "icons/settings_icon.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl data_model opcua_client navigation settings wifi_widget trends
)
