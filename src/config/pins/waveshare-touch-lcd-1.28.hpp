// https://www.waveshare.com/product/rp2040-touch-lcd-1.28.htm

#pragma once

#include "config/pins.hpp"

constexpr GPIO_Pin PIN_FAN_PWM = 28;
constexpr GPIO_Pin PIN_FAN_TACHOMETER = 27;
constexpr GPIO_Pin PIN_NEOPIXEL_DATA_IN = 26;

constexpr GPIO_Pin PINS_I2C[] = {
        16, 17,  // I2C 0
        6, 7,    // I2C 1, hardwired to touch device
};

constexpr GPIO_Pin PINS_DISPLAY_SPI[] = {10, 11, 12};
// TODO: they specify a LCD CS on GPIO9, might need to pull-down/pull up on that?
constexpr GPIO_Pin PIN_DISPLAY_COMMAND = 8;
constexpr GPIO_Pin PIN_DISPLAY_RESET = 13;
constexpr GPIO_Pin PIN_DISPLAY_BRIGHTNESS = 25;
constexpr GPIO_Pin PIN_TOUCH_INTERRUPT = 21;
constexpr GPIO_Pin PIN_TOUCH_RESET = 22;

constexpr std::array<GPIO_Pin, 0> PINS_RESERVED_BOARD;
