#pragma once

/**
 * @file pinled_machine_config.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief The device-wide runtime configuration record.
 * @version 0.4.0
 * @date 2026-08-10
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * The companion to `ChannelConfig`: that one is the runtime model for a single
 * channel, this one is the runtime model for the device. Both are projections
 * of the same document model (`proto/pinled.proto`), and both are flat PODs
 * with no strings, no pointers and no IDF types.
 *
 * @par Why it moved out of machine_config.h
 * It lived next to the store, which includes `driver/gpio.h` and `esp_err.h`
 * and so cannot be compiled off-target. That made `install_to_machine()` —
 * ordinary field-by-field logic with a real "what does zero mean" question in
 * it — untestable except by flashing a board. This is the same move made for
 * `DriveClass` in step 2 and `FilamentParams` in step 3, for the same reason:
 * a rule deserves ordinary tests, so the type it operates on has to be
 * reachable from the host.
 *
 * @par Pins are int32_t, not gpio_num_t
 * `gpio_num_t` is an IDF type, and the negative values that matter here are
 * already meaningful without it: -1 is `GPIO_NUM_NC`, and a negative Kconfig
 * value has always meant "not fitted" for `/PL` on a bench rig. The store casts
 * at the point of use.
 */

#include <cstddef>
#include <cstdint>

#include "pinled_color_order.h"

namespace ooe::pinled
{
    /// A pin that is not fitted. Numerically `GPIO_NUM_NC`.
    inline constexpr int32_t kPinNotFitted = -1;

    struct MachineConfig
    {
        // --- scan geometry ---
        int32_t clk_pin{kPinNotFitted};  ///< chain CLK, driven as SPI SCLK
        int32_t data_pin{kPinNotFitted}; ///< serial DATA stream, read as SPI MISO
        int32_t pl_pin{kPinNotFitted};   ///< bussed '165 parallel load (active low)
        int32_t led_pin{kPinNotFitted};
        int32_t spi_hz{2 * 1000 * 1000}; ///< chain clock rate
        uint8_t spi_mode{2};             ///< 2 = sample mid bit-cell ('165 shifts on rising)
        bool pl_from_cs{true};           ///< drive /PL from SPI CS
        size_t num_modules{1};
        size_t channels_per_module{16};
        bool active_low{false}; ///< non-inverting front end (HW-1); see docs/TIMING.md §4.2

        // --- timing ---
        float sample_rate_hz{2000.0f}; ///< per-channel raw sample rate target
        uint32_t refresh_hz{90};       ///< LED frame rate

        // --- filament defaults (profiler and per-lamp overrides may replace) ---
        float attack_ms{30.0f};
        float decay_ms{40.0f};
        float gamma{2.2f};

        // --- LED string ---
        size_t led_count{16};
        /// Byte order the strip expects. Default GRB: standard WS2812B, and
        /// what every install behaved as before the field existed.
        ColorOrder color_order{ColorOrder::UNSPECIFIED};

        size_t total_channels() const { return num_modules * channels_per_module; }
    };
} // namespace ooe::pinled
