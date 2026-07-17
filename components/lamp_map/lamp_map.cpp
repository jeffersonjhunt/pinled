/**
 * @file lamp_map.cpp
 * @brief Channel -> LED mapping and WS2812B rendering (first-cut).
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * TODO(v1): per-lamp tint tables loaded from machine_config, low-level
 * dithering, multi-LED groups per channel, brightness cap for PSU budget.
 */

#include "lamp_map.h"

#include <new>

#include "esp_log.h"
#include "neopixel.h"

namespace ooe::pinled
{
    static const char *TAG = "lamp_map";

    static inline uint8_t scale8(uint8_t value, uint8_t scale)
    {
        return static_cast<uint8_t>((static_cast<uint16_t>(value) * scale) / 255);
    }

    esp_err_t LampMap::init(const LampMapConfig &cfg)
    {
        if (cfg.led_pin == GPIO_NUM_NC || cfg.led_count == 0 || cfg.channel_count == 0)
            return ESP_ERR_INVALID_ARG;

        deinit();
        cfg_ = cfg;

        map_ = new (std::nothrow) LampMapEntry[cfg_.channel_count];
        if (!map_)
            return ESP_ERR_NO_MEM;

        tNeopixelContext ctx = neopixel_Init(cfg_.led_count, cfg_.led_pin);
        if (ctx == nullptr)
        {
            ESP_LOGE(TAG, "neopixel_Init failed (pin %d, count %u)",
                     (int)cfg_.led_pin, (unsigned)cfg_.led_count);
            deinit();
            return ESP_FAIL;
        }
        neopixel_ = ctx;

        set_default_mapping();
        initialized_ = true;
        ESP_LOGI(TAG, "init: %u LEDs on GPIO %d, %u channels",
                 (unsigned)cfg_.led_count, (int)cfg_.led_pin, (unsigned)cfg_.channel_count);
        return ESP_OK;
    }

    void LampMap::deinit()
    {
        if (neopixel_)
        {
            neopixel_Deinit(static_cast<tNeopixelContext>(neopixel_));
            neopixel_ = nullptr;
        }
        delete[] map_;
        map_ = nullptr;
        initialized_ = false;
    }

    void LampMap::set_default_mapping()
    {
        for (size_t ch = 0; ch < cfg_.channel_count; ++ch)
        {
            LampMapEntry &e = map_[ch];
            e.led_index = (ch < cfg_.led_count) ? static_cast<int16_t>(ch) : -1;
            e.r = 255; // warm-white-ish base; real tints come from config
            e.g = 200;
            e.b = 140;
        }
    }

    esp_err_t LampMap::set_entry(size_t channel, const LampMapEntry &e)
    {
        if (channel >= cfg_.channel_count)
            return ESP_ERR_INVALID_ARG;
        map_[channel] = e;
        return ESP_OK;
    }

    esp_err_t LampMap::render(const uint8_t *levels, size_t n)
    {
        if (!initialized_)
            return ESP_ERR_INVALID_STATE;
        if (!levels)
            return ESP_ERR_INVALID_ARG;

        const size_t count = n < cfg_.channel_count ? n : cfg_.channel_count;

        // Build the frame. One pixel per mapped channel; unmapped channels skip.
        for (size_t ch = 0; ch < count; ++ch)
        {
            const LampMapEntry &e = map_[ch];
            if (e.led_index < 0 || static_cast<size_t>(e.led_index) >= cfg_.led_count)
                continue;
            const uint8_t v = levels[ch];
            tNeopixel px{
                static_cast<uint32_t>(e.led_index),
                NP_RGB(scale8(e.r, v), scale8(e.g, v), scale8(e.b, v))};
            neopixel_SetPixel(static_cast<tNeopixelContext>(neopixel_), &px, 1);
        }
        return ESP_OK;
    }
} // namespace ooe::pinled
