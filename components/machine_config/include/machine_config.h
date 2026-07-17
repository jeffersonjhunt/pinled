#pragma once

/**
 * @file machine_config.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief NVS-backed machine profiles and build-time defaults.
 * @version 0.2.0
 * @date 2026-07-16
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * Holds the runtime configuration for a given machine: pin assignments, module
 * geometry, sampling/refresh rates, LED count, and (v1) the per-channel color
 * map and profiler locks. Boots to Kconfig defaults when no profile is stored.
 *
 * STATUS: first-cut skeleton. Defaults-from-Kconfig and the struct are done;
 * NVS load/save and JSON import/export are stubbed (v1 / future).
 */

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "driver/gpio.h"

namespace ooe::pinled
{
    struct MachineConfig
    {
        // --- scan geometry ---
        gpio_num_t clk_pin{GPIO_NUM_NC};
        gpio_num_t mr_pin{GPIO_NUM_NC};
        gpio_num_t data_pins[8]{};
        gpio_num_t led_pin{GPIO_NUM_NC};
        size_t num_modules{1};
        size_t channels_per_module{16};
        bool active_low{true};

        // --- timing ---
        float sample_rate_hz{2000.0f}; ///< per-channel raw sample rate target
        uint32_t refresh_hz{90};       ///< LED frame rate

        // --- filament defaults (profiler may override per channel) ---
        float attack_ms{30.0f};
        float decay_ms{40.0f};
        float gamma{2.2f};

        // --- LED string ---
        size_t led_count{16};

        size_t total_channels() const { return num_modules * channels_per_module; }
    };

    class MachineConfigStore
    {
    public:
        /// Initialize NVS and load the active profile, or fall back to Kconfig
        /// build defaults if none is stored.
        esp_err_t load(MachineConfig &out);

        /// Persist the given config as the active profile. TODO(v1).
        esp_err_t save(const MachineConfig &cfg);

        /// Fill a config from Kconfig (CONFIG_PINLED_*) build defaults.
        static MachineConfig defaults();
    };
} // namespace ooe::pinled
