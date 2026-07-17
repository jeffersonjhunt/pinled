#pragma once

/**
 * @file lamp_map.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief Channel -> LED mapping and WS2812B/SK6812 rendering.
 * @version 0.2.0
 * @date 2026-07-16
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * Relates each sensed lamp channel to one or more addressable-LED indices with
 * a base color/tint, multiplies the base color by the channel's reconstructed
 * brightness, and pushes a frame over RMT (zorxx/neopixel, as in the POC).
 *
 * STATUS: first-cut skeleton. 1:1 channel->LED mapping and color multiply are
 * implemented; per-lamp tint tables, dithering, and multi-LED groups are v1.
 */

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "driver/gpio.h"

namespace ooe::pinled
{
    struct LampMapEntry
    {
        int16_t led_index{-1};   ///< target LED, -1 = unmapped
        uint8_t r{255};          ///< base color (multiplied by brightness)
        uint8_t g{255};
        uint8_t b{255};
    };

    struct LampMapConfig
    {
        gpio_num_t led_pin{GPIO_NUM_NC};
        size_t led_count{0};
        size_t channel_count{0};
    };

    class LampMap
    {
    public:
        esp_err_t init(const LampMapConfig &cfg);
        void deinit();

        /// Default 1:1 mapping: channel i -> LED i, warm white base.
        void set_default_mapping();
        esp_err_t set_entry(size_t channel, const LampMapEntry &e);

        /// Render one frame from per-channel brightness (0..255).
        esp_err_t render(const uint8_t *levels, size_t n);

        size_t led_count() const { return cfg_.led_count; }

    private:
        LampMapConfig cfg_{};
        LampMapEntry *map_{nullptr};
        void *neopixel_{nullptr}; ///< tNeopixelContext (opaque here)
        bool initialized_{false};
    };
} // namespace ooe::pinled
