idf_component_register(
    SRCS
        "app_main.c"
        "doomgeneric_esp32p4.c"
        "compat_newlib.c"
        "recv_wad.c"
    INCLUDE_DIRS "."
    REQUIRES doomgeneric esp_timer esp_psram fatfs esp_driver_usb_serial_jtag
    # The vendored doomgeneric archive pulls DG_* / system() from here; force
    # every object in this component to link so archive scan order can't drop
    # doomgeneric_esp32p4.o or compat_newlib.o.
    WHOLE_ARCHIVE)
