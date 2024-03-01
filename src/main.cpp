#include "FreeRTOS.h"  // IWYU pragma: keep
#include "btstack_run_loop.h"
#include "config.hpp"
#include "display.hpp"
#include "gatt.hpp"
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/platform_defs.h"
#include "pico/cyw43_arch.h"
#include "pico/stdio.h"
#include "sdk/i2c.hpp"
#include "sdk/spi.hpp"
#include "sensors.hpp"
#include "settings.hpp"
#include "task.h"  // IWYU pragma: keep
#include "utility/i2c.hpp"
#include "utility/task.hpp"
#include "utility/timer.hpp"
#include "ws2812.hpp"
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <utility>

#ifndef NDEBUG
#include "utility/square_wave.hpp"
#endif

using namespace std;
using namespace nevermore;

extern "C" {
void vApplicationTickHook() {}

void vApplicationStackOverflowHook(TaskHandle_t Task, char* pcTaskName) {
    panic("PANIC - stack overflow in task %s\n", pcTaskName);
}

void vApplicationMallocFailedHook() {
    panic("PANIC - heap alloc failed\n");
}
}

namespace {

// Leave pins {0, 1} set to UART TX/RX.
// Clear everything else.
void pins_clear_user_defined() {
    for (GPIO_Pin pin = 2; pin < PIN_MAX; ++pin) {
        if (find(begin(PINS_RESERVED_BOARD), end(PINS_RESERVED_BOARD), pin) != end(PINS_RESERVED_BOARD))
            continue;

        gpio_set_function(pin, GPIO_FUNC_NULL);
        gpio_set_dir(pin, false);
        gpio_pull_down(pin);
    }
}

// so far PIN config can be statically checked, so no risk of runtime error
void pins_setup() {
    for (auto pin : PINS_I2C) {
        gpio_set_function(pin, GPIO_FUNC_I2C);
        gpio_pull_up(pin);
    }

    gpio_set_function(PIN_FAN_PWM, GPIO_FUNC_PWM);
    gpio_set_function(PIN_FAN_TACHOMETER, GPIO_FUNC_PWM);
    gpio_pull_up(PIN_FAN_TACHOMETER);

    // we're setting up the WS2812 controller on PIO0
    gpio_set_function(PIN_NEOPIXEL_DATA_IN, GPIO_FUNC_PIO0);

    for (auto pin : PINS_DISPLAY_SPI)
        gpio_set_function(pin, GPIO_FUNC_SPI);

    gpio_set_function(PIN_DISPLAY_COMMAND, GPIO_FUNC_SIO);
    gpio_set_function(PIN_DISPLAY_RESET, GPIO_FUNC_SIO);
    gpio_set_function(PIN_DISPLAY_BRIGHTNESS, GPIO_FUNC_PWM);
    gpio_set_function(PIN_TOUCH_INTERRUPT, GPIO_FUNC_SIO);
    gpio_set_function(PIN_TOUCH_RESET, GPIO_FUNC_SIO);

    gpio_set_dir(PIN_DISPLAY_COMMAND, true);
    gpio_set_dir(PIN_DISPLAY_RESET, true);
    gpio_set_dir(PIN_TOUCH_INTERRUPT, false);
    gpio_set_dir(PIN_TOUCH_RESET, true);

#ifndef NDEBUG
    if (PIN_DBG_SQUARE_WAVE) {
        // setup a debug
        square_wave_pwm_init(*PIN_DBG_SQUARE_WAVE, 30);
    } else
        printf("!! No available PWM slice for square wave generator.\n");
#endif
}

// NB: changes pin function assignments
void pins_i2c_reset() {
    auto get = [](uint8_t bus, I2C_Pin kind) {
        for (auto pin : PINS_I2C)
            if (i2c_gpio_bus_num(pin) == bus && i2c_gpio_kind(pin) == kind) return pin;

        unreachable();
    };

    static_assert(size(PINS_I2C) == 4, "too many pins - not impl");
    for (uint8_t i = 0; i < NUM_I2CS; ++i) {
        // `i2c_bitbang_reset` is responsible for changing the pin functions
        if (!i2c_bitbang_reset(get(i, I2C_Pin::SDA), get(i, I2C_Pin::SCL)))
            printf("WARN - I2C%d - failed to reset bus\n", i);
    }
}

}  // namespace

int main() {
    stdio_init_all();
    adc_init();

    nevermore::settings::init();

    pins_clear_user_defined();
    pins_i2c_reset();           // bit-bang out a reset for the I2C buses
    pins_clear_user_defined();  // clear pins again, `pins_i2c_reset` leaves things dirty
    pins_setup();               // setup everything (except UART, which should be set to default 0/1)

    // GCC 12.2.1 bug: -Werror=format reports that `I2C_BAUD_RATE` is a `long unsigned int`.
    // This is technically true on this platform, see static-assert below, but it is benign since
    // `unsigned == long unsigned int` is also true on this platform. Pedant.
    // Fix by casting to unsigned instead of changing format specifier, this keeps clangd happy
    // since `clangd`, incorrectly, thinks that `unsigned != long unsigned int` on this platform.
    static_assert(sizeof(I2C_BAUD_RATE) == sizeof(unsigned));
    printf("I2C bus 0 running at %u baud/s (requested %u baud/s)\n", i2c_init(i2c0, I2C_BAUD_RATE),
            unsigned(I2C_BAUD_RATE));
    printf("I2C bus 1 running at %u baud/s (requested %u baud/s)\n", i2c_init(i2c1, I2C_BAUD_RATE),
            unsigned(I2C_BAUD_RATE));

    auto* spi = spi_gpio_bus(PINS_DISPLAY_SPI[0]);
    printf("SPI bus %d running at %u baud/s (requested %u baud/s)\n", spi_gpio_bus_num(PINS_DISPLAY_SPI[0]),
            spi_init(spi, SPI_BAUD_RATE_DISPLAY), unsigned(SPI_BAUD_RATE_DISPLAY));

    mk_task("startup", Priority::Startup, 1024)([]() {
        if constexpr (std::string_view(PICO_BOARD) == "pico_w") {
            // need the CYW43 up to access the LED, even if we don't have BT enabled
            if (auto err = cyw43_arch_init()) {
                panic("ERR - cyw43_arch_init failed = 0x%08x\n", err);
            }
        }

        ws2812::init();
        if (!gatt::init()) return;
        // display must be init before sensors b/c some sensors are display input devices
        if (!display::init_with_ui()) return;
        if (!sensors::init()) return;

        mk_timer("led-blink", SENSOR_UPDATE_PERIOD)([](TimerHandle_t) {
            static bool led_on = false;
            led_on = !led_on;
            if constexpr (std::string_view(PICO_BOARD) == "pico_w") {
                // HACK:  `cyw43_arch_gpio_put` w/o having the HCI powered on
                //        kills the timer task when it enters `cyw43_ensure_up`.
                //        Root cause unknown. This hack should be benign since
                //        Pico W is typically built w/ BT enabled.
                if constexpr (NEVERMORE_PICO_W_BT) {
                    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_on);
                }
            } else {
                // TODO:  Make this configurable/generalisable to other boards?
                // constexpr uint8_t PICO_LED_PIN = 25;
                // gpio_put(PICO_LED_PIN, led_on);
            }
        });

        if constexpr (NEVERMORE_PICO_W_BT) {
            mk_task("bluetooth", Priority::Communication, 1024)(btstack_run_loop_execute).release();
        }

        vTaskDelete(nullptr);  // we're done, delete ourselves
    }).release();

    vTaskStartScheduler();  // !! NO-RETURN
    return 0;
}
