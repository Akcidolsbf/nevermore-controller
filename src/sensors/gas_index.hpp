#pragma once

#include "lib/sensirion_gas_index_algorithm.h"
#include "sensors.hpp"
#include "settings.hpp"
#include <cassert>
#include <chrono>
#include <cstring>

namespace nevermore::sensors {

struct GasIndex {
    using Clock = std::chrono::steady_clock;

    static constexpr auto CHECKPOINT_PERIOD = 24h;

    GasIndexAlgorithmParams gia{};
    Clock::time_point next_checkpoint = Clock::now() + CHECKPOINT_PERIOD;

    GasIndex(int32_t type = GasIndexAlgorithm_ALGORITHM_TYPE_VOC,
            settings::Settings const& settings = settings::g_active) {
        assert(type == GasIndexAlgorithm_ALGORITHM_TYPE_VOC || type == GasIndexAlgorithm_ALGORITHM_TYPE_NOX);
        GasIndexAlgorithm_init(&gia, type);

        if (settings.voc_gating != BLE::NOT_KNOWN) {
            assert(1 <= settings.voc_gating && settings.voc_gating <= 500);
            gia.mGating_Threshold = F16(settings.voc_gating.value_or(0));
        }
    }

    VOCIndex process(int32_t raw) {
        int32_t voc_index{};
        GasIndexAlgorithm_process(&gia, raw, &voc_index);
        assert(0 <= voc_index && voc_index <= 500);
        return voc_index;
    }

    // returns false IFF `src` doesn't contain a saved state
    bool restore(settings::SensorCalibrationBlob const& src) {
        Blob blob;
        static_assert(sizeof(Blob) <= sizeof(src));
        memcpy(&blob, &src, sizeof(Blob));
        if (blob[0] == 0 && blob[1] == 0) return false;

        GasIndexAlgorithm_set_states(&gia, blob[0], blob[1]);
        next_checkpoint = Clock::now() + CHECKPOINT_PERIOD;
        return true;
    }

    void save(settings::SensorCalibrationBlob& dst) {
        Blob blob;
        GasIndexAlgorithm_get_states(&gia, &blob[0], &blob[1]);
        static_assert(sizeof(Blob) <= sizeof(dst));
        memcpy(&dst, &blob, sizeof(Blob));
    }

    bool checkpoint(settings::SensorCalibrationBlob& blob) {
        auto const now = Clock::now();
        if (now < next_checkpoint) return false;

        next_checkpoint = now + CHECKPOINT_PERIOD;
        save(blob);
        return true;
    }

private:
    using Blob = int32_t[2];
};

}  // namespace nevermore::sensors
