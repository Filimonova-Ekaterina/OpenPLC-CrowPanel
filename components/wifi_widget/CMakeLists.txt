set(WIFI_ICON_SRC
    icons/wifi_off.c
    icons/wifi_no_internet.c
    icons/wifi_low.c
    icons/wifi_medium.c
    icons/wifi_medium_high.c
    icons/wifi_high.c
    )

set(SRC
    src/wifi_widget.c
    )

set(INC
    include
    icons/
    )

idf_component_register(
    SRCS ${SRC} ${WIFI_ICON_SRC}
    INCLUDE_DIRS ${INC}
    REQUIRES
        lvgl
        settings
    )
