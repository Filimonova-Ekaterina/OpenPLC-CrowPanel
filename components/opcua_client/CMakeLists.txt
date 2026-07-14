idf_component_register(
    SRCS "src/opcua_client.c"
    INCLUDE_DIRS "include"
    REQUIRES data_model settings esp_netif freertos open62541
)
