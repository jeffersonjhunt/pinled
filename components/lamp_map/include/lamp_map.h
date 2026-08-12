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
 * Rendering builds the **whole strip** into a frame buffer and issues a single
 * strip transmit per refresh (FR-LED-6). That is not just an optimization: the
 * underlying driver silently drops SetPixel calls that arrive closer together
 * than one strip time, so the previous one-call-per-channel loop lost most of
 * its updates and left LEDs showing stale values.
 *
 * STATUS: 1:1 channel->LED mapping and color multiply are implemented; per-lamp
 * tint tables, dithering, and multi-LED groups are v1.
 */

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

#include "driver/gpio.h"

#include "pinled_color_order.h"

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
        /// Byte order the strip expects (FR-CFG-6). Default GRB — standard
        /// WS2812B, and what this driver emits natively.
        ColorOrder color_order{ColorOrder::UNSPECIFIED};
    };

    class LampMap
    {
    public:
        esp_err_t init(const LampMapConfig &cfg);
        void deinit();

        /// Default 1:1 mapping: channel i -> LED i, warm white base.
        void set_default_mapping();
        esp_err_t set_entry(size_t channel, const LampMapEntry &e);

        /**
         * @brief Render one frame from per-channel brightness (0..255).
         *
         * Builds every LED in the strip and transmits once (FR-LED-6). LEDs
         * with no channel mapped to them are driven dark, so the output is a
         * pure function of `levels[]` rather than of what was shown before.
         */
        esp_err_t render(const uint8_t *levels, size_t n);

        /**
         * @brief Walk a single lit pixel along the whole string, in order.
         *
         * A power-on proof that every LED is present, wired the right way
         * round, and reachable — end to end, before any lamp has been sensed.
         * A WS2812 string is a shift register in disguise: one dead pixel or
         * one bad joint kills everything after it, and without this the
         * symptom is "the far half of my playfield does nothing", which reads
         * as a configuration fault and is not one.
         *
         * Deliberately bypasses the channel map. This tests the STRING, not
         * the wiring — an LED with no channel mapped to it must still light
         * here, or the test would only ever confirm what is already
         * configured.
         *
         * @param ms_per_led dwell per LED; 0 skips the walk entirely
         *
         * @note One pixel at a time, not a cumulative fill. A full string of
         *       white is ~60 mA a pixel — 7.7 A at the 128-LED maximum, which
         *       would brown out most bench supplies. A single dot is 60 mA
         *       whatever the string length.
         * @note Blocks for `led_count * ms_per_led`. Called before the render
         *       task starts, so nothing is competing for the strip.
         */
        esp_err_t walk(uint32_t ms_per_led);

        /**
         * @brief Drive the entire string to one colour, bypassing the map.
         *
         * For load measurement and bring-up. Like `walk`, this is about the
         * string rather than the wiring, so every pixel lights whether or not
         * a channel points at it.
         *
         * @warning At full white this is the worst-case current the string can
         *          draw. Know your supply before calling it on a long string.
         */
        esp_err_t fill(uint8_t r, uint8_t g, uint8_t b);

        size_t led_count() const { return cfg_.led_count; }

        /// Highest refresh the driver can actually display, in **Hz**, given
        /// the strip length. Used to validate `refresh_hz` at boot (FR-LED-8).
        ///
        /// Note the upstream header documents this as "minimum ticks between
        /// calls"; the implementation returns `bitrate / (bufferSize * 8)`,
        /// which is a frequency. Verified against the source, not the comment.
        uint32_t max_refresh_hz() const;

    private:
        LampMapConfig cfg_{};
        LampMapEntry *map_{nullptr};
        void *neopixel_{nullptr}; ///< tNeopixelContext (opaque here)
        void *frame_{nullptr};    ///< tNeopixel[led_count], built once per refresh
        bool initialized_{false};
    };
} // namespace ooe::pinled
