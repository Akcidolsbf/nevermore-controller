#include "sgp40.hpp"
#include "async_sensor.hpp"
#include "sdk/ble_data_types.hpp"
#include "sdk/i2c.hpp"
#include "sdk/timer.hpp"
#include "utility/misc.hpp"
#include "utility/numeric_suffixes.hpp"
#include "utility/packed_tuple.hpp"
#include <cstdint>
#include <utility>

extern "C" {
#include "utility/sensirion_gas_index_algorithm.h"
}

namespace {

constexpr uint8_t SGP40_ADDRESS = 0x59;

// SGP40 wants its cmds in BE order
constexpr auto CMD_SGP40_SELF_TEST = byteswap(0x280E_u16);      // available in all modes, doesn't change mode
constexpr auto CMD_SGP40_MEASURE = byteswap(0x260F_u16);        // transitions to measure mode
constexpr auto CMD_SGP4x_HEATER_OFF = byteswap(0x3615_u16);     // transitions to idle mode
constexpr auto CMD_SGP4x_SERIAL_NUMBER = byteswap(0x3682_u16);  // only available when in idle mode

bool sgp4x_heater_off(i2c_inst_t* bus) {
    if (!bus) return false;
    return 2 == i2c_write_blocking(bus, SGP40_ADDRESS, CMD_SGP4x_HEATER_OFF);
}

// returns true IIF self-test passed. any error (I2C or self-test) -> false
bool sgp40_self_test(i2c_inst_t* bus) {
    if (!bus) return false;
    if (2 != i2c_write_blocking(bus, SGP40_ADDRESS, CMD_SGP40_SELF_TEST)) return false;

    sleep(320ms);  // spec says max delay of 320ms

    auto response = i2c_read_blocking_crc<0xFF, uint8_t, uint8_t>(bus, SGP40_ADDRESS);
    if (!response) return false;

    auto&& [code, _] = *response;
    switch (code) {
        case 0xD4: return true;   // tests passed
        case 0x4B: return false;  // tests failed
    }

    printf("WARN - unexpected response code from SGP40 self-test: 0x%02x\n", int(code));
    return false;
}

bool sgp40_measure_issue(i2c_inst_t* bus, double temperature, double humidity) {
    if (!bus) return {};

    auto to_tick = [](double n, double min, double max) {
        return byteswap(uint16_t((clamp(n, min, max) - min) / (max - min) * UINT16_MAX));
    };

    auto temperature_tick = to_tick(temperature, -45, 130);
    auto humidity_tick = to_tick(humidity, 0, 100);
    PackedTuple cmd{CMD_SGP40_SELF_TEST, temperature_tick, crc8(temperature_tick, 0xFF), humidity_tick,
            crc8(humidity_tick, 0xFF)};
    return sizeof(cmd) == i2c_write_blocking(bus, SGP40_ADDRESS, cmd);
}

bool sgp40_measure_issue(
        i2c_inst_t* bus, BLE::Temperature const& temperature, BLE::Humidity const& humidity) {
    return sgp40_measure_issue(bus, temperature.value_or(25), humidity.value_or(50));
}

std::optional<uint16_t> sgp40_measure_read(i2c_inst_t* bus) {
    if (!bus) return {};

    auto response = i2c_read_blocking_crc<0xFF, uint16_t>(bus, SGP40_ADDRESS);
    if (!response) return false;

    auto&& [voc_raw] = *response;
    return byteswap(voc_raw);
}

bool sgp40_exists(i2c_inst_t* bus) {
    return sgp4x_heater_off(bus);  // could
}

struct SGP40 : SensorDelayedResponse {
    i2c_inst_t* bus;
    Sensor::Data data;  // tiny bit wasteful, but terser to manage
    GasIndexAlgorithmParams gas_index_algorithm{};

    SGP40(i2c_inst_t* bus, Sensor::Data data) : bus(bus), data(std::move(data)) {
        GasIndexAlgorithm_init(&gas_index_algorithm, GasIndexAlgorithm_ALGORITHM_TYPE_VOC);
    }

    [[nodiscard]] char const* name() const override {
        return "SGP40";
    }

    [[nodiscard]] std::chrono::milliseconds read_delay() const override {
        return 320ms;
    }

    bool issue() override {
        return sgp40_measure_issue(bus, std::get<BLE::Temperature&>(data), std::get<BLE::Humidity&>(data));
    }

    void read() override {
        auto voc_raw = sgp40_measure_read(bus);
        if (!voc_raw) {
            printf("SGP40 - read back failed\n");
            return;
        }

        // ~330 us during steady-state, ~30 us during startup blackout
        int32_t gas_index{};
        GasIndexAlgorithm_process(&gas_index_algorithm, *voc_raw, &gas_index);
        assert(0 <= gas_index && gas_index <= 500 && "result out of range?");
        if (gas_index == 0) return;  // 0 -> index not available

        std::get<EnvironmentService::VOCIndex&>(data) = gas_index;
    }
};

}  // namespace

std::unique_ptr<SensorPeriodic> sgp40(i2c_inst_t* bus, Sensor::Data state) {
    if (!sgp40_exists(bus)) return {};  // nothing found
    if (!sgp40_self_test(bus)) {
        printf("Found SGP40, but failed self-test\n");
        return {};
    }

    return std::make_unique<SGP40>(bus, state);
}
