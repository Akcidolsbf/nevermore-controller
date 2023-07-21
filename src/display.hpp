#pragma once

#include "hardware/spi.h"

namespace nevermore::display {

// Initialises the display and the UI.
bool init_with_ui(spi_inst_t&);

void brightness(float power);  // range: [0, 1]
float brightness();            // range: [0, 1]

struct Resolution {
    uint16_t width;
    uint16_t height;
};

constexpr Resolution RESOLUTION{
        .width = 240,
        .height = 240,
};

};  // namespace nevermore::display
