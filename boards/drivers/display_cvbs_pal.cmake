set(SOLAR_OS_BOARD_DISPLAY_DRIVER "cvbs_pal")
list(APPEND SOLAR_OS_BOARD_SRCS
    "board/solar_os_board_display_cvbs_pal.c"
    "drivers/cvbs_pal.c"
)
list(APPEND SOLAR_OS_BOARD_REQUIRES
    esp_driver_gpio
    esp_hw_support
    esp_rom
    hal
    u8g2
)
