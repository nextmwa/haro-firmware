idf_component_register(
    SRCS "haro_wifi_provisioning.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_wifi esp_netif esp_event network_provisioning
)
