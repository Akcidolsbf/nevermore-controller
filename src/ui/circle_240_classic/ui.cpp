// This file was generated by SquareLine Studio
// SquareLine Studio version: SquareLine Studio 1.3.4
// LVGL version: 8.3.6
// Project name: circle-240-classic

#include "ui.hpp"

#if LV_COLOR_DEPTH != 16
#error "LV_COLOR_DEPTH should be 16bit to match SquareLine Studio's settings"
#endif
#if LV_COLOR_16_SWAP != 1
#error "LV_COLOR_16_SWAP should be 1 to match SquareLine Studio's settings"
#endif

extern "C" {
NevermoreDisplayUI nevermore_ui_circle_240_create_screen();
}

namespace nevermore::ui::circle_240_classic {

NevermoreDisplayUI create() {
    auto* dispp = lv_disp_get_default();
    auto* theme = lv_theme_default_init(
            dispp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED), true, LV_FONT_DEFAULT);
    lv_disp_set_theme(dispp, theme);

    auto ui = nevermore_ui_circle_240_create_screen();
    lv_disp_load_scr(ui.screen);
    return ui;
}

}  // namespace nevermore::ui::circle_240_classic
