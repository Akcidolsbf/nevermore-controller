#pragma once

#include <array>
#include <bit>
#include <cassert>
#include <climits>
#include <cmath>
#include <compare>
#include <cstdint>
#include <limits>
#include <type_traits>

static_assert(CHAR_BIT == 8, "Neat, you've a real weird architecture. Good luck.");
static_assert(std::endian::native == std::endian::little, "Implementation assumes system is little endian.");

//////////////////////////////////////////////
// Basic Data Types
//////////////////////////////////////////////

namespace BLE {

struct [[gnu::packed]] uint24_t {
    std::array<uint8_t, 3> octets{};

    constexpr uint24_t() = default;

    constexpr uint24_t(uint8_t u1, uint8_t u2, uint8_t u3) {
        octets[0] = u1;
        octets[1] = u2;
        octets[2] = u3;
    }

    constexpr uint24_t(uint16_t n) {
        octets[0] = n & 0xFF;
        octets[1] = (n >> 8) & 0xFF;
        octets[2] = (n >> 16) & 0xFF;
    }

    constexpr uint24_t(uint32_t n) {
        assert(n <= 0xFFFFFF && "value out of range for uint24_t");
        octets[0] = n & 0xFF;
        octets[1] = (n >> 8) & 0xFF;
        octets[2] = (n >> 16) & 0xFF;
    }

    constexpr uint24_t(int n) {
        assert(0 <= n && n <= 0xFFFFFF && "value out of range for uint24_t");
        octets[0] = n & 0xFF;
        octets[1] = (n >> 8) & 0xFF;
        octets[2] = (n >> 16) & 0xFF;
    }

    explicit constexpr uint24_t(double n) {
        assert(0 <= n && n <= 0xFFFFFF && "value out of range for uint24_t");
        *this = uint24_t(uint32_t(n));
    }

    constexpr operator uint32_t() const {
        return octets[0] | (octets[1] >> 8) | (octets[2] >> 16);
    }
};
static_assert(sizeof(uint24_t) == 3);

namespace internal {
template <typename T, bool IS_SIGNED>
struct numeric_limits_integer {
    static constexpr bool is_specialized = true;
    static constexpr bool is_signed = IS_SIGNED;
    static constexpr bool is_exact = true;
    static constexpr bool has_infinity = false;
    static constexpr bool has_quiet_NaN = false;
    static constexpr bool has_signaling_NaN = false;
    static constexpr bool has_denorm = false;
    static constexpr bool has_denorm_loss = false;
    static constexpr auto round_style = std::round_toward_zero;
    static constexpr int digits = CHAR_BIT * sizeof(T);
    static constexpr int digits10 = digits * std::log10(2);
    static constexpr int radix = 2;
    static constexpr int min_exponent = 0;
    static constexpr int min_exponent10 = 0;
    static constexpr int max_exponent = 0;
    static constexpr int max_exponent10 = 0;
    static constexpr bool traps = true;  // TODO: use whatever this platform uses for ints
    static constexpr bool tinyness_before = false;

    static constexpr T max() noexcept;     // specialiser must define/override
    static constexpr T min() noexcept;     // specialiser must define/override
    static constexpr T lowest() noexcept;  // specialiser must define/override

    static constexpr T epsilon() noexcept {
        return {};
    };
    static constexpr T round_error() noexcept {
        return {};
    };
    static constexpr T infinity() noexcept {
        return {};
    };
    static constexpr T quiet_NaN() noexcept {
        return {};
    };
    static constexpr T signaling_NaN() noexcept {
        return {};
    };
    static constexpr T denorm_min() noexcept {
        return {};
    };
};
}  // namespace internal

}  // namespace BLE

template <>
struct std::numeric_limits<BLE::uint24_t> : BLE::internal::numeric_limits_integer<BLE::uint24_t, false> {
    static constexpr BLE::uint24_t max() noexcept {
        return {0xFF, 0xFF, 0xFF};
    };
    static constexpr BLE::uint24_t min() noexcept {
        return {};
    };
    static constexpr BLE::uint24_t lowest() noexcept {
        return {};
    };
};

namespace BLE {

//////////////////////////////////////////////
// Common Scalar Types
//////////////////////////////////////////////

struct NOT_KNOWN {
} constexpr NOT_KNOWN;

namespace internal {

template <typename A>
struct NOT_KNOWN_VALUE {};

template <typename A>
concept has_not_known = requires {
    { NOT_KNOWN_VALUE<A>::value };
};

constexpr double raw_to_repr_coeff(int M, int d, int b) {
    // HACK:  clangd doesn't believe `std::pow` is `constexpr`.
    //        Define a custom impl to suppress spurious errors.
    auto pow = [](double base, int i) -> double {
        double r = 1;
        for (; i < 0; ++i)
            r /= base;
        for (; i > 0; --i)
            r *= base;
        return r;
    };

    return M * pow(10., d) * pow(2., b);
}

template <typename Unit, typename Raw_, int32_t M_ = 1, int32_t D = 0, int32_t B = 0>
struct [[gnu::packed]] Scalar {
    using Raw = Raw_;

    static_assert(M_ != 0, "M must be != 0");
    static_assert(-10 <= M_ && M_ <= 10, "valid range [-10, 10] as per GATT spec supplement");

    static constexpr int32_t M = M_;
    static constexpr int32_t d = D;
    static constexpr int32_t b = B;
    static constexpr double scale = raw_to_repr_coeff(M, d, b);

    static constexpr Scalar from_raw(Raw raw) {
        Scalar x{0};  // zero init to avoid default ctor & constexpr lifetime ordering issue
        x.raw_value = raw;
        return x;
    }

    Raw raw_value;

    constexpr Scalar() {
        if constexpr (has_not_known<Scalar>)
            raw_value = NOT_KNOWN_VALUE<Scalar>::value.raw_value;
        else
            raw_value = {};
    }

    constexpr Scalar(struct NOT_KNOWN const&)
    // requires has_not_known<Scalar> /* require disable b/c of GCC 11+ bug */
    {
        *this = NOT_KNOWN_VALUE<Scalar>::value;
    }

    constexpr Scalar(double value) : raw_value(static_cast<Raw>(value / scale)) {}

    constexpr explicit operator double() const {
        if constexpr (has_not_known<Scalar>) {
            if (*this == NOT_KNOWN) return std::numeric_limits<double>::signaling_NaN();
        }

        return raw_value * scale;
    }

    [[nodiscard]] constexpr double value_or(double x) const
    // requires has_not_known<Scalar> /* require disable b/c of GCC 11+ bug */
    {
        return *this == NOT_KNOWN ? x : double(*this);
    }

    // sadly, because we're not doing `<=> = default`, we don't get the free definitions for <, ==, etc..
    constexpr bool operator==(Scalar const&) const = default;
};

template <typename Unit, typename Raw, int32_t M, int32_t D, int32_t B>
constexpr auto operator<(Scalar<Unit, Raw, M, D, B> const& lhs, Scalar<Unit, Raw, M, D, B> const& rhs) {
    if constexpr (has_not_known<Scalar<Unit, Raw, M, D, B>>) {
        if (lhs == NOT_KNOWN || rhs == NOT_KNOWN) return false;
    }

    return lhs.raw_value < rhs.raw_value;
}

template <typename Unit, typename Raw, int32_t M, int32_t D, int32_t B>
constexpr auto operator<=>(Scalar<Unit, Raw, M, D, B> const& lhs, Scalar<Unit, Raw, M, D, B> const& rhs) {
    if constexpr (has_not_known<Scalar<Unit, Raw, M, D, B>>) {
        if (lhs.raw_value == rhs.raw_value) return std::partial_ordering::equivalent;
        if (lhs == NOT_KNOWN || rhs == NOT_KNOWN) return std::partial_ordering::unordered;

        return std::partial_ordering(lhs.raw_value <=> rhs.raw_value);
    } else {
        return lhs.raw_value <=> rhs.raw_value;
    }
}

}  // namespace internal

using internal::has_not_known;

#define BLE_DECLARE_SCALAR_TYPE(name, raw, M, d, e) \
    using name = BLE::internal::Scalar<struct name##_unit_t__, raw, M, d, e>

#define BLE_DECLARE_NOT_KNOWN_VALUE(type, expr)   \
    template <>                                   \
    struct BLE::internal::NOT_KNOWN_VALUE<type> { \
        constexpr static type value = expr;       \
    };

#define BLE_DECLARE_NOT_KNOWN_VALUE_FROM_RAW(type, raw) \
    BLE_DECLARE_NOT_KNOWN_VALUE(type, type::from_raw(raw));

BLE_DECLARE_SCALAR_TYPE(Count16, uint16_t, 1, 0, 0);    // range [0, 65534]
BLE_DECLARE_SCALAR_TYPE(Humidity, uint16_t, 1, -2, 0);  // range [0.00, 100.00] %
// why is this so big? who the hell is measuring 424 atmospheres of pressure on cheap BLE sensors?!
BLE_DECLARE_SCALAR_TYPE(Pressure, uint32_t, 1, -1, 0);    // range [0, 429496729.5] Pa
BLE_DECLARE_SCALAR_TYPE(Percentage8, uint8_t, 1, 0, -1);  // range [0, 100] %, 0.5 % increment
BLE_DECLARE_SCALAR_TYPE(Temperature, int16_t, 1, -2, 0);  // range [-273.15, 327.67] c

BLE_DECLARE_NOT_KNOWN_VALUE_FROM_RAW(Count16, 0xFFFFu);
BLE_DECLARE_NOT_KNOWN_VALUE_FROM_RAW(Humidity, 0xFFFFu);
BLE_DECLARE_NOT_KNOWN_VALUE_FROM_RAW(Percentage8, 0xFFu);
BLE_DECLARE_NOT_KNOWN_VALUE_FROM_RAW(Temperature, 0x8000);

constexpr Pressure PRESSURE_1_ATMOSPHERE{101.325 * 1000};  // 101.325 kPa

//////////////////////////////////////////////
// Common Utility Characteristics
//////////////////////////////////////////////

// range is inclusive
template <typename T>
struct [[gnu::packed]] ValidRange {
    T min = {};   // minimum valid value, inclusive
    T max = min;  // maximum valid value, inclusive
};

//////////////////////////////////////////////
// Standard Profile Specific Characteristics
//////////////////////////////////////////////

struct [[gnu::packed]] EnvironmentalSensorMeasurementDesc {
    enum class Sampling : uint8_t {
        Unspecified = 0x00,
        Instantaneous = 0x01,
        ArithmeticMean = 0x02,
        RMS = 0x03,
        Maximum = 0x04,
        Minimum = 0x05,
        Accumulated = 0x06,
        Count = 0x07,
    };

    // TODO: there are more defined applications
    enum class Application : uint8_t {
        Unspecified = 0x00,
        Air = 0x01,
        Water = 0x02,
        Barometric = 0x03,
        Supplementary = 0x1A,
        Internal = 0x1F,
        External = 0x20,
    };

    BLE_DECLARE_SCALAR_TYPE(Seconds, uint24_t, 1, 0, 0);

    uint16_t flags = 0;           // reserved, must be zero
    Sampling sampling = Sampling::Unspecified;
    Seconds measure_period = 0;   // 0 -> unused/instant
    Seconds update_interval = 0;  // 0 -> not in use
    Application application = Application::Unspecified;
    // base 2, exponent -1, percentage. TODO: add/use a template for odd bases/exponents
    Percentage8 uncertainty = NOT_KNOWN;
};
static_assert(sizeof(EnvironmentalSensorMeasurementDesc) == 11);

BLE_DECLARE_NOT_KNOWN_VALUE(EnvironmentalSensorMeasurementDesc::Application,
        EnvironmentalSensorMeasurementDesc::Application::Unspecified);
BLE_DECLARE_NOT_KNOWN_VALUE(EnvironmentalSensorMeasurementDesc::Sampling,
        EnvironmentalSensorMeasurementDesc::Sampling::Unspecified);

};  // namespace BLE
