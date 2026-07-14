idf_component_register(
    SRCS
        "esp32_p4_function_ev_board.c"
        "my_codec.c"
        "bsp_stc8h1kxx.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES driver sdmmc
    PRIV_REQUIRES
        esp_lcd
        usb
        spiffs
        fatfs
        esp_driver_uart
        esp_driver_i2s
        esp_driver_ledc
)
