#include "ens16x.hpp"
#include "config.hpp"
#include "sdk/ble_data_types.hpp"
#include "sensors.hpp"
#include "utility/i2c_device.hpp"
#include "utility/numeric_suffixes.hpp"
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <optional>

using namespace std;
using namespace BLE;

namespace nevermore::sensors {

static_assert(
        I2C_BAUD_RATE <= 1'000'000, "`config.hpp`'s `I2C_BAUD_RATE` is too high for ENS16x (max 1 M/s)");

namespace {

constexpr array ADDRESSES{0x52_u8, 0x53_u8};

enum class Reg : uint8_t {
    PartID = 0x00u,  // 16 bits
    OpMode = 0x10u,
    Config = 0x11u,
    Command = 0x12u,
    TempIn = 0x13u,
    RelHumidityIn = 0x15u,
    DataStatus = 0x20u,
    DataAqiUBI = 0x21u,
    DataTVOC = 0x22u,
    DataECO2 = 0x24u,
    DataAqiScioSense = 0x26u,
    DataReserved0 = 0x28u,  // 10 octets (??)
    DataTemperature = 0x30u,
    DataRelativeHumidity = 0x32u,  // 16 bits
    DataChecksum = 0x38u,          // AKA `MISR`
    GprWrite0 = 0x40u,             // up to 8 octets
    GprRead0 = 0x48u,              // up to 8 octets
    GprRead4 = GprRead0 + 4,
};

enum class OpMode : uint8_t {
    DeepSleep = 0x00u,
    Idle = 0x01u,
    Operational = 0x02u,
    Reset = 0xF0u,
};

// Commands can only be executed in Idle mode
enum class Cmd : uint8_t {
    NoOp = 0x00u,
    GetAppVersion = 0x0Eu,
    ClearGPR = 0xCCu,
};

// enum values must match official part IDs and must be a classed to `uint16_t`
enum class Kind : uint16_t { ENS160 = 0x0160u, ENS161 = 0x0161u };

struct [[gnu::packed]] AppVersion {
    uint8_t major, minor, revision;
};

struct [[gnu::packed]] Status {
    enum { Normal = 0, WarmUp = 1, StartUp = 2, Invalid = 3 };

    uint8_t new_gpr : 1;
    uint8_t new_data : 1;
    uint8_t validity : 2;  // 0 = normal, 1 = warm-up, 2 = startup, 3 = invalid
    uint8_t _reserved : 2;
    uint8_t error : 1;   // 0 = normal, 1 = error detected
    uint8_t statas : 1;  // (sic) "High indicates that an OPMODE is running" -> mode change in progress?
};
static_assert(sizeof(Status) == sizeof(uint8_t));

struct [[gnu::packed]] Compensation {
    uint16_t temperature;
    uint16_t humidity;
};

struct MISR {
    uint8_t expected = 0;  // mirror of DATA_MISR (0 is hardware default)

    template <typename A>
    void update(A const& value) {
        auto const* ptr = reinterpret_cast<uint8_t const*>(&value);
        auto const* end = ptr + sizeof(A);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        for (; ptr < end; ++ptr)            // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            expected = apply(expected, *ptr);
    }

private:
    static uint8_t apply(uint8_t miso, uint8_t data) {
        constexpr auto POLY = 0x1D;  // 0b00011101 = x^8+x^4+x^3+x^2+x^0 (x^8 is implicit)
        uint8_t misr_xor = ((miso << 1) ^ data) & 0xFF;
        if ((miso & 0x80) == 0) return misr_xor;

        return misr_xor ^ POLY;
    }
};

struct ENS16xSensor final : SensorPeriodic {
    I2CDevice<Reg, "ENS16x"> i2c;
    EnvironmentalFilter side;
    Kind kind = Kind::ENS160;  // assume we're the simpler one until we test
    MISR misr;

    ENS16xSensor(i2c_inst_t& bus, uint8_t address, EnvironmentalFilter side)
            : i2c{bus, address}, side(side) {}

    [[nodiscard]] char const* name() const override {
        return "ENS16x";
    }

    bool setup() {
        if (!mode(OpMode::Reset, true)) return false;
        if (!mode(OpMode::Idle)) return false;

        auto kind = read_kind();
        if (!kind) return false;
        this->kind = *kind;
        printf("ENS16x - kind: %u\n", unsigned(this->kind));

        auto version = read_app_version();
        if (!version) return false;
        printf("ENS16x - version: %d.%d.%d\n", version->major, version->minor, version->revision);

        return mode(OpMode::Operational);
    }

    void read() override {
        Compensation compensation{
                .temperature = uint16_t(max(0., (side.compensation_humidity() + 273.15) * 64)),
                .humidity = uint16_t(side.compensation_humidity() * 512),
        };
        if (!i2c.write(Reg::TempIn, compensation)) return;

        // Data* calls must be read via `read_crc` to update checksum
        auto status = read_data_verified<Status>(Reg::DataStatus);
        if (!status) {
            printf("ERR - ENS16x - failed to fetch status\n");
            return;
        }
        if (!status->new_data) return;  // nothing to read

        if (status->validity == Status::Invalid) {
            printf("ERR - ENS16x - invalid status for read\n");
            return;
        }

        // Serendipitously, this sensor also offers an arbitrary AQI value in the range of [0, 500]
        auto aqi_level = read_data_verified<uint16_t>(Reg::DataAqiScioSense);
        if (!aqi_level) {
            printf("ERR - ENS16x - failed to read AQI-ScioSense\n");
            return;
        }

        side.set(VOCIndex(clamp<uint16_t>(*aqi_level, 1, 500)));
    }

    bool mode(OpMode mode, bool quiet = false) {  // NOLINT(readability-make-member-function-const)
        if (!i2c.write(Reg::OpMode, mode)) {
            if (!quiet) {
                printf("ERR - ENS16x - failed to change to mode=%02x\n", uint8_t(mode));
            }

            return false;
        }
        // HACK:  Give the device a moment to switch.
        //        If you don't, the checksum read can fail, and without that
        //        you can't read the status.
        task_delay(ENS16x_POWER_ON_DELAY);

        // Reset does *NOT* clear/set MISR. Have to query the current state from the device.
        // Might as well do this now when we're changing modes.
        auto curr_misr = i2c.read<uint8_t>(Reg::DataChecksum);
        if (!curr_misr) {
            printf("ERR - ENS16x - failed to sync checksum\n");
            return false;
        }
        misr.expected = *curr_misr;

        // `statas` is low when mode change is complete
        return status_await([](auto& x) { return !x.statas; });
    }

    optional<Kind> read_kind() {
        auto part_id = read_data_verified<uint16_t>(Reg::PartID);
        if (!part_id) {
            printf("ERR - ENS16x - failed to read part ID\n");
            return {};
        }

        switch ((Kind)*part_id) {
        case Kind::ENS160:  // FALL THROUGH
        case Kind::ENS161: return (Kind)*part_id; break;
        default: {
            printf("ERR - ENS16x - unrecognised part ID 0x%04x\n", *part_id);
            return {};
        } break;
        }
    }

    optional<AppVersion> read_app_version() {
        // clear GPR to ensure new-gpr is triggered
        if (!i2c.write(Reg::Command, Cmd::ClearGPR)) {
            printf("ERR - ENS16x - failed to send cmd `ClearGPR`\n");
            return {};
        }

        if (!i2c.write(Reg::Command, Cmd::GetAppVersion)) {
            printf("ERR - ENS16x - failed to send cmd `GetAppVersion`\n");
            return {};
        }

        if (!status_await([](auto& x) { return x.new_gpr; })) return {};

        auto version = read_data_verified<AppVersion>(Reg::GprRead4);
        if (!version) {
            printf("ERR - ENS16x - failed to read version\n");
            return {};
        }

        return version;
    }

    // NB:  Datasheet says registers in [0x20, 0x37] trigger a MSIR update.
    //      This is a lie. It looks like *every* read updates MISR,
    //      *except* the MSIR register itself.
    template <typename A>
    optional<A> read_data(Reg reg) {
        auto x = i2c.read<A>(reg);
        if (!x) return {};

        misr.update(*x);
        return x;
    }

    template <typename A>
    optional<A> read_data_verified(Reg reg) {
        auto x = read_data<A>(reg);
        if (x && !misr_verify()) return {};
        return x;
    }

    [[nodiscard]] optional<Status> status() {
        return read_data_verified<Status>(Reg::DataStatus);
    }

    template <typename F>
    bool status_await(F&& filter) {
        for (;;) {
            auto status = this->status();
            if (!status) return false;
            if (filter(*status)) return true;
            vTaskYieldWithinAPI();
        }
    }

    bool misr_verify() {
        auto actual = i2c.read<uint8_t>(Reg::DataChecksum);
        if (!actual) {
            printf("ERR - ENS16x - failed to read checksum\n");
            return false;
        }

        if (misr.expected != actual) {
            printf("WARN - ENS16x - checksum mismatch. expected=0x%02x actual=0x%02x\n", misr.expected,
                    *actual);
            misr.expected = *actual;  // sync w/ actual previous value
            return false;
        }

        return true;
    }
};

unique_ptr<SensorPeriodic> ens16x(uint8_t address, i2c_inst_t& bus, EnvironmentalFilter side) {
    auto p = make_unique<ENS16xSensor>(bus, address, side);
    if (!p->setup()) return {};  // no sensor or failed to set up

    return p;
}

}  // namespace

unique_ptr<SensorPeriodic> ens16x(i2c_inst_t& bus, EnvironmentalFilter side) {
    for (auto address : ADDRESSES)
        if (auto p = ens16x(address, bus, side)) return p;

    return {};
}

}  // namespace nevermore::sensors
