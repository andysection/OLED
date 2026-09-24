idf_component_register(
    SRCS
        "main.c"
        "pan_netif.c"
        "internet_time.c"
        "internet_http.c"
        "bt_app_core_utils.c"
    PRIV_REQUIRES
        bt
        nvs_flash
        esp_netif
        esp_event
        esp_http_client
        lwip
    INCLUDE_DIRS "."
)
