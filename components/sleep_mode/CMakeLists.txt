idf_component_register(SRCS "src/sleep_mode.c" INCLUDE_DIRS
                            "include" REQUIRES freertos espressif__esp32_p4_function_ev_board esp_event)