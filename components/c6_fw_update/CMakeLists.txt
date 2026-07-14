idf_component_register(
    SRCS "c6_fw_update.c"
    INCLUDE_DIRS "include"
    EMBED_FILES "network_adapter_esp32c6.bin"
    PRIV_REQUIRES
        bootloader_support
        esp_app_format
        esp_hosted
        esp_system
)
