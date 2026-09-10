idf_component_register(
    SRCS "server_client.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_websocket_client protocol
)
