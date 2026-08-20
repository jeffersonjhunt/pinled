/**
 * @file lamp_map.cpp
 * @brief Channel -> LED mapping and WS2812B rendering (first-cut).
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * TODO(v1): per-lamp tint tables loaded from machine_config, low-level
 * dithering, multi-LED groups per channel, brightness cap for PSU budget.
 */

#include "lamp_map.h"

#include "pinled_channel_config.h" // ResolveDefaults — the one definition of the default tint
#include "pinled_color_order.h"

#include <cmath>
#include <new>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

        // The same shape as the filament bank's level LUT, and deliberately
        // the same gamma figure: one knob. Identity when gamma is 1 or
        // nonsense, so a linear build renders exactly what it is given.
        const float gamma = (cfg_.gamma > 0.0f) ? cfg_.gamma : 1.0f;
        for (int i = 0; i < 256; ++i)
        {
            const float y = std::pow(static_cast<float>(i) / 255.0f, gamma);
            tint_lut_[i] = static_cast<uint8_t>(y * 255.0f + 0.5f);
        }

        map_ = new (std::nothrow) LampMapEntry[cfg_.channel_count];
        if (!map_)
            return ESP_ERR_NO_MEM;

        // Allocated once here, never in the render path (NFR-4).
        frame_ = new (std::nothrow) tNeopixel[cfg_.led_count];
        if (!frame_)
        {
            deinit();
            return ESP_ERR_NO_MEM;
        }

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
        ESP_LOGI(TAG, "init: %u LEDs on GPIO %d, %u channels, 1 transmit/frame",
                 (unsigned)cfg_.led_count, (int)cfg_.led_pin, (unsigned)cfg_.channel_count);
        ESP_LOGI(TAG, "  strip supports up to %u Hz refresh, byte order %s",
                 (unsigned)max_refresh_hz(), color_order_str(cfg_.color_order));
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
        delete[] static_cast<tNeopixel *>(frame_);
        frame_ = nullptr;
        initialized_ = false;
    }

    void LampMap::set_default_mapping()
    {
        for (size_t ch = 0; ch < cfg_.channel_count; ++ch)
        {
            LampMapEntry &e = map_[ch];
            // One definition of the default tint, shared with the config layer
            // (ResolveDefaults). This used to be three literals here, and they
            // had drifted from the config layer's copy — which would have
            // restyled every unconfigured lamp the moment boot started routing
            // through apply_channel_config().
            static constexpr ResolveDefaults kDefaults{};
            e.led_index = (ch < cfg_.led_count) ? static_cast<int16_t>(ch) : -1;
            // Through the same LUT as set_entry: the default tint is a
            // swatch too, and it rendered washed-out for the same reason.
            e.r = tint_lut_[kDefaults.r];
            e.g = tint_lut_[kDefaults.g];
            e.b = tint_lut_[kDefaults.b];
        }
    }

    esp_err_t LampMap::walk(uint32_t ms_per_led)
    {
        if (!initialized_)
            return ESP_ERR_INVALID_STATE;
        if (ms_per_led == 0)
            return ESP_OK;

        ESP_LOGI(TAG, "startup walk: %u LEDs at %u ms (%u ms total)",
                 (unsigned)cfg_.led_count, (unsigned)ms_per_led,
                 (unsigned)(cfg_.led_count * ms_per_led));

        tNeopixel *frame = static_cast<tNeopixel *>(frame_);

        // White, so all three elements are exercised. It is also the one colour
        // that looks identical whatever byte order the strip uses, which is
        // correct here: this answers "is the pixel alive", and the fixture in
        // fs_seed answers "is the order right".
        const uint32_t lit = pack_for_order(cfg_.color_order, 255, 255, 255);

        for (size_t on = 0; on < cfg_.led_count; ++on)
        {
            for (size_t i = 0; i < cfg_.led_count; ++i)
            {
                frame[i].index = static_cast<uint32_t>(i);
                frame[i].rgb = (i == on) ? lit : 0;
            }
            neopixel_SetPixel(static_cast<tNeopixelContext>(neopixel_),
                              frame, static_cast<uint32_t>(cfg_.led_count));
            vTaskDelay(pdMS_TO_TICKS(ms_per_led));
        }

        // Leave the string dark rather than holding the last pixel on: the
        // next thing to touch it is the render task, and a stuck pixel between
        // the two would look like a fault this test had just caused.
        for (size_t i = 0; i < cfg_.led_count; ++i)
        {
            frame[i].index = static_cast<uint32_t>(i);
            frame[i].rgb = 0;
        }
        neopixel_SetPixel(static_cast<tNeopixelContext>(neopixel_),
                          frame, static_cast<uint32_t>(cfg_.led_count));
        return ESP_OK;
    }

    esp_err_t LampMap::fill(uint8_t r, uint8_t g, uint8_t b)
    {
        if (!initialized_)
            return ESP_ERR_INVALID_STATE;

        tNeopixel *frame = static_cast<tNeopixel *>(frame_);
        const uint32_t rgb = pack_for_order(cfg_.color_order, r, g, b);
        for (size_t i = 0; i < cfg_.led_count; ++i)
        {
            frame[i].index = static_cast<uint32_t>(i);
            frame[i].rgb = rgb;
        }
        return neopixel_SetPixel(static_cast<tNeopixelContext>(neopixel_),
                                 frame, static_cast<uint32_t>(cfg_.led_count))
                   ? ESP_OK
                   : ESP_FAIL;
    }

    esp_err_t LampMap::set_entry(size_t channel, const LampMapEntry &e)
    {
        if (channel >= cfg_.channel_count)
            return ESP_ERR_INVALID_ARG;
        // Stored linearised — see the header. render() then multiplies two
        // linear quantities, and the swatch someone picked is what they get.
        LampMapEntry lin = e;
        lin.r = tint_lut_[e.r];
        lin.g = tint_lut_[e.g];
        lin.b = tint_lut_[e.b];
        map_[channel] = lin;
        return ESP_OK;
    }

    uint32_t LampMap::max_refresh_hz() const
    {
        if (!initialized_)
            return 0;
        return neopixel_GetRefreshRate(static_cast<tNeopixelContext>(neopixel_));
    }

    esp_err_t LampMap::render(const uint8_t *levels, size_t n)
    {
        if (!initialized_)
            return ESP_ERR_INVALID_STATE;
        if (!levels)
            return ESP_ERR_INVALID_ARG;

        tNeopixel *frame = static_cast<tNeopixel *>(frame_);

        // Build the whole strip, dark by default, so an LED with no channel
        // mapped to it is driven off rather than left showing whatever it had.
        for (size_t i = 0; i < cfg_.led_count; ++i)
        {
            frame[i].index = static_cast<uint32_t>(i);
            frame[i].rgb = 0;
        }

        const size_t count = n < cfg_.channel_count ? n : cfg_.channel_count;
        for (size_t ch = 0; ch < count; ++ch)
        {
            const LampMapEntry &e = map_[ch];
            if (e.led_index < 0 || static_cast<size_t>(e.led_index) >= cfg_.led_count)
                continue;
            const uint8_t v = levels[ch];
            // NOT NP_RGB: that macro hard-codes the driver's native GRB
            // transmission order, which is wrong on an RGB-ordered strip. The
            // packing is the same 24-bit layout, chosen by the install's
            // declared order — see pinled_color_order.h for why the byte sent
            // first lives in the middle of the word.
            frame[e.led_index].rgb =
                pack_for_order(cfg_.color_order, scale8(e.r, v), scale8(e.g, v), scale8(e.b, v));
        }

        // ONE transmit per refresh (FR-LED-6). The driver drops SetPixel calls
        // spaced closer than a strip time, so the old per-channel loop was
        // discarding most of its own updates, not merely wasting time.
        const bool ok = neopixel_SetPixel(static_cast<tNeopixelContext>(neopixel_),
                                          frame, static_cast<uint32_t>(cfg_.led_count));
        return ok ? ESP_OK : ESP_FAIL;
    }
} // namespace ooe::pinled
