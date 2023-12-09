#include "sensors.hpp"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "sdk/ble_data_types.hpp"
#include "sensors/ahtxx.hpp"
#include "sensors/async_sensor.hpp"
#include "sensors/bme280.hpp"
#include "sensors/bme68x.hpp"
#include "sensors/cst816s.hpp"
#include "sensors/ens16x.hpp"
#include "sensors/environmental.hpp"
#include "sensors/htu2xd.hpp"
#include "sensors/sgp30.hpp"
#include "sensors/sgp40.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

using namespace std;

namespace nevermore::sensors {

Sensors g_sensors;
Config g_config;

namespace {

constexpr uint32_t ADC_CHANNEL_TEMP_SENSOR = 4;

constexpr auto SENSOR_POWER_ON_DELAY = max({
        AHTxx_POWER_ON_DELAY,
        BME280_POWER_ON_DELAY,
        BME68x_POWER_ON_DELAY,
        ENS16x_POWER_ON_DELAY,
        HTU21D_POWER_ON_DELAY,
        SGP30_POWER_ON_DELAY,
        SGP40_POWER_ON_DELAY,
});

using VecSensors = vector<unique_ptr<Sensor>>;

VecSensors g_sensors_intake;
VecSensors g_sensors_exhaust;

struct McuTemperature final : SensorPeriodic {
    [[nodiscard]] char const* name() const override {
        return "MCU Temperature";
    }

    void read() override {
        nevermore::sensors::g_sensors.temperature_mcu = measure();
    }

private:
    static double measure() {
        // ref https://github.com/raspberrypi/pico-micropython-examples/blob/master/adc/temperature.py
        constexpr auto SCALE_COEFFICIENT = 3.3 / 65535;
        constexpr uint32_t BITS = 12;

        adc_select_input(ADC_CHANNEL_TEMP_SENSOR);
        uint32_t raw32 = adc_read();
        // Scale raw reading to 16 bit value using a Taylor expansion (for 8 <= bits <= 16)
        uint16_t raw16 = raw32 << (16 - BITS) | raw32 >> (2 * BITS - 16);
        auto reading = raw16 * SCALE_COEFFICIENT;
        // The temp sensor measures the Vbe voltage of a biased bipolar diode, connected to ADC channel 4.
        // Typically, Vbe = 0.706V at 27c, with a slope of -1.721mV (0.001721) per degree.
        auto deg_c = 27 - (reading - 0.706) / 0.001721;
        return deg_c;
    }
} g_mcu_temperature_sensor;

VecSensors sensors_init_bus(i2c_inst_t& bus, EnvironmentalFilter state) {
    VecSensors sensors;
    auto probe_for = [&](auto p) {
        if (!p) return;
        printf("Found %s\n", p->name());
        p->start();
        sensors.push_back(std::move(p));
    };

    probe_for(ahtxx(bus, state));
    probe_for(bme280(bus, state));
    probe_for(bme68x(bus, state));
    probe_for(ens16x(bus, state));
    probe_for(htu2xd(bus, state));
    probe_for(sgp30(bus, state));
    probe_for(sgp40(bus, state));
    probe_for(CST816S::mk(bus));

    if (sensors.empty()) printf("!! No sensors found?\n");
    return sensors;
}

}  // namespace

Sensors Sensors::with_fallbacks(Config const& config) const {
    EnvironmentalFilter intake{EnvironmentalFilter::Kind::Intake};
    EnvironmentalFilter exhaust{EnvironmentalFilter::Kind::Exhaust};
    auto apply = [&]<typename A>(A& x, EnvironmentalFilter side) { x = side.get<A>(*this, config); };
    Sensors sensors = *this;
    apply(sensors.temperature_intake, intake);
    apply(sensors.humidity_intake, intake);
    apply(sensors.pressure_intake, intake);
    apply(sensors.voc_index_intake, intake);
    apply(sensors.temperature_exhaust, exhaust);
    apply(sensors.humidity_exhaust, exhaust);
    apply(sensors.pressure_exhaust, exhaust);
    apply(sensors.voc_index_exhaust, exhaust);
    return sensors;
}

bool init() {
    adc_select_input(ADC_CHANNEL_TEMP_SENSOR);
    adc_set_temp_sensor_enabled(true);
    g_mcu_temperature_sensor.start();

    // Explicitly reset b/c we may be restarting the program w/o power cycling the device.
    CST816S::reset_all();

    printf("Waiting %u ms for sensor init\n", unsigned(SENSOR_POWER_ON_DELAY / 1ms));
    task_delay(SENSOR_POWER_ON_DELAY);

    printf("I2C0 - initializing sensors...\n");
    g_sensors_intake = sensors_init_bus(*i2c0, {EnvironmentalFilter::Kind::Intake});

    printf("I2C1 - initializing sensors...\n");
    g_sensors_exhaust = sensors_init_bus(*i2c1, {EnvironmentalFilter::Kind::Exhaust});

    // wait again b/c probing might be implemented by sending a reset command to the sensor
    task_delay(SENSOR_POWER_ON_DELAY);

    return true;
}

}  // namespace nevermore::sensors
