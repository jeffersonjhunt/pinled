#pragma once

/**
 * @file lamp_scan.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief 74HC161 + 74HC251 lamp-channel scan driver.
 * @version 0.2.0
 * @date 2026-07-16
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * Drives a 74HC161 synchronous counter whose Q0..Q2 select the channel on one
 * or two 74HC251 tri-state 8:1 muxes (Q3 bank-selects between the two '251s on
 * a shared DATA_IN line, giving 16 channels per module). CLK and /MR are a
 * shared bus across all modules; each module returns its bit on a dedicated
 * DATA_IN GPIO, so a scan frame reads every module in lockstep.
 *
 * This is the bit-bang implementation carried forward from the POC. The public
 * API is backend-agnostic; a timer / dedic_gpio clock source can replace the
 * inner loop later without changing callers.
 */

#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "esp_err.h"

namespace ooe::pinled
{
    static constexpr size_t LAMPSCAN_MAX_MODULES = 8;

    struct LampScanConfig
    {
        gpio_num_t clk_pin{GPIO_NUM_NC};   ///< 74HC161 CLK (shared)
        gpio_num_t mr_pin{GPIO_NUM_NC};    ///< 74HC161 /MR async clear (shared, active low)
        gpio_num_t data_pins[LAMPSCAN_MAX_MODULES]{}; ///< one DATA_IN per module
        size_t num_modules{1};             ///< number of modules on the bus
        size_t channels_per_module{16};    ///< 8 (single '151/'251) or 16 (dual '251)
        bool active_low{true};             ///< inverting FET front end -> true
        uint32_t settle_ns{50};            ///< mux/counter settle before sampling
    };

    class LampScan
    {
    public:
        /// Total sensed channels = num_modules * channels_per_module.
        static constexpr size_t MAX_CHANNELS = LAMPSCAN_MAX_MODULES * 16;

        esp_err_t init(const LampScanConfig &cfg);

        /**
         * @brief Read one full scan frame into out[].
         *
         * Layout is module-major: out[module * channels_per_module + channel].
         * @param out    caller buffer of booleans (already polarity-corrected)
         * @param n      capacity of out; must be >= total_channels()
         */
        esp_err_t read_frame(bool *out, size_t n);

        /// Convenience: read channel index i of module m within the current
        /// frame position (advances the counter). Prefer read_frame().
        size_t total_channels() const { return cfg_.num_modules * cfg_.channels_per_module; }

        /// Pulse /MR low to zero the counter (async clear -> count 0).
        void reset_counter();

    private:
        inline void clock_pulse();
        inline bool sample(size_t module);

        LampScanConfig cfg_{};
        bool initialized_{false};
    };
} // namespace ooe::pinled
