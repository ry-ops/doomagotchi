idf_component_register(
    SRCS
        "main.c"
        "wifi_sniff.c"
        "pcap_wad.c"
    INCLUDE_DIRS "." "../../proto"
    REQUIRES esp_wifi nvs_flash esp_timer esp_event
             fatfs sdmmc esp_driver_sdspi esp_driver_spi esp_driver_gpio)
