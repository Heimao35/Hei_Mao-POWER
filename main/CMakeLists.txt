file(GLOB_RECURSE srcs *.c
                    ui/menu/*.c
                    )

# 时钟相关源文件保留在仓库中但不参与编译
list(FILTER srcs EXCLUDE REGEX ".*/(app_time|rtc_pcf85063|ui_main_clock)\\.c$")

list(APPEND srcs
    "ui/menu/ui_status_bar.c"
    "ui/menu/ui_power_app.c"
    "ui/menu/ui_power_main.c"
    "ui/menu/ui_power_sheet.c"
    "ui/menu/ui_pd_panel.c"
    "ui/menu/ui_menu_brightness.c"
    "ui/menu/ui_menu_wifi.c"
    "ui/menu/ui_menu_volume.c"
    "i2c_bus_share.c"
    "power_meter.c"
    "ina236.c"
    "pd_spoof.c"
    "audio_volume.c"
    "buzzer.c"
)
list(REMOVE_DUPLICATES srcs)

set(include_dirs
    .
    ui/menu
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${include_dirs}
    PRIV_REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_lcd
        esp_timer
        esp_driver_ledc
        driver
        lvgl__lvgl
        esp_lcd_co5300
        esp_lcd_touch_cst820
        esp_lcd_sh8601
    )

add_compile_definitions(LV_LVGL_H_INCLUDE_SIMPLE)
