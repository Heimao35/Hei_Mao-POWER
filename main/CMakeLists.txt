file(GLOB_RECURSE srcs *.c
                    *.cpp
                    ui/custom/*.c
                    ui/generated/*.c
                    ui/generated/guider_customer_fonts/*.c
                    ui/generated/guider_fonts/*.c
                    ui/generated/images/*.c
                    )

# ESP-IDF cannot use CONFIGURE_DEPENDS here (component requirements pass); list explicitly so new files always link.
list(APPEND srcs "ui/menu/ui_status_bar.c")
list(APPEND srcs "ui/menu/ui_main_clock.c")
list(APPEND srcs "app_time.c")
list(APPEND srcs "rtc_pcf85063.c")
list(APPEND srcs "i2c_bus_share.c")
list(APPEND srcs "axp2101_battery.c")
list(REMOVE_DUPLICATES srcs)

set(include_dirs
    .
    ui/menu
    ui/custom
    ui/generated
    ui/generated/guider_customer_fonts
    ui/generated/images
)
# LVGL Guider may omit guider_fonts when no built-in fonts are exported; empty dirs are not in Git, so only add if present.
if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ui/generated/guider_fonts")
    list(APPEND include_dirs ui/generated/guider_fonts)
endif()

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
        esp_driver_rmt
        esp_driver_ledc
        driver
        lvgl__lvgl
        esp_lcd_co5300
        esp_lcd_touch_cst820
        esp_lcd_sh8601
    )

add_compile_definitions(LV_LVGL_H_INCLUDE_SIMPLE)