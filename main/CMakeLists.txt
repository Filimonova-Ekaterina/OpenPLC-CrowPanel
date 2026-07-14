idf_component_register(
    SRCS
        "main.cpp"
        "idle_monitor.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_event
        esp_netif
        espressif__esp32_p4_function_ev_board
        settings
        sleep_mode
        data_model
        opcua_client
        ui_generator
        navigation
        c6_fw_update
)
