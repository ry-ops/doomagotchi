idf_component_register(
    SRCS
        "main.c"
        "wifi_sniff.c"
    INCLUDE_DIRS "." "../../proto"
    REQUIRES esp_wifi nvs_flash esp_timer esp_event)
