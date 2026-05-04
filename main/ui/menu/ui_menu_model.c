/**
 * @file ui_menu_model.c
 */
#include "ui_menu_model.h"

static const ui_menu_main_item_t s_menu[UI_MENU_MAIN_COUNT] = {
    {
        .title = "System",
        .sub = { "Wi-Fi", "Bluetooth", "Power", "RGB" },
    },
    {
        .title = "Display",
        .sub = { "Brightness", "Theme", "Timeout", "Auto" },
    },
    {
        .title = "Audio",
        .sub = { "Volume", "Equalizer", "Output", "Input" },
    },
    {
        .title = "Sensors",
        .sub = { "Calibration", "Logging", "Units", "Rate" },
    },
    {
        .title = "About",
        .sub = { "Device", "Firmware", "Legal", "Credits" },
    },
};

const ui_menu_main_item_t *ui_menu_model_get_table(void)
{
    return s_menu;
}

uint32_t ui_menu_model_main_count(void)
{
    return UI_MENU_MAIN_COUNT;
}
