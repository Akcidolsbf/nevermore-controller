#include "fan_policy.hpp"
#include "gatt/environmental.hpp"
#include "sdk/ble_data_types.hpp"
#include <chrono>
#include <cmath>
#include <utility>

using namespace std;
using namespace std::literals::chrono_literals;
using namespace BLE;
using EnvironmentService::VOCIndex;

namespace {

bool should_filter(FanPolicyEnvironmental const& params, VOCIndex intake, VOCIndex exhaust) {
    // Can't decide anything until we have readings available
    if (intake == BLE::NOT_KNOWN) return false;
    if (exhaust == BLE::NOT_KNOWN) return false;

    // Too filthy in here. Just start filtering.
    if (params.voc_passive_max <= std::max(intake, exhaust)) return true;

    auto const voc_improvement = intake.value_or(0) - exhaust.value_or(0);
    return params.voc_improve_min.value_or(INFINITY) < voc_improvement;
}

}  // namespace

float FanPolicyEnvironmental::Instance::operator()(
        EnvironmentService::ServiceData const& state, chrono::system_clock::time_point now) {
    // can't do anything until we have readings available
    if (state.voc_index_intake == BLE::NOT_KNOWN) return 0;
    if (state.voc_index_exhaust == BLE::NOT_KNOWN) return 0;

    if (should_filter(params, state.voc_index_intake, state.voc_index_exhaust)) {
        last_filtered = chrono::system_clock::now();
        return 1;  // conditions are bad enough we should filter
    }

    if (chrono::system_clock::now() <
            last_filtered + chrono::seconds(uint32_t(params.cooldown.value_or(0)))) {
        return 1;  // in cooldown phase, keep going for a bit to mop up the leftovers
    }

    return 0;
}
