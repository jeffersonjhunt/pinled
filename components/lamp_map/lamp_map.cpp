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
#include <cstring>
#include <new>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "ws2812b.h"

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
        frame_ = new (std::nothrow) uint8_t[cfg_.led_count * 3];
        if (!frame_)
        {
            deinit();
            return ESP_ERR_NO_MEM;
        }

        // The driver moves bytes; THIS component decides them. Colour order
        // is applied here via bytes_for_order (host-tested), so the driver's
        // own order setting is irrelevant — everything goes through
        // write_raw, which transmits verbatim. DMA on: the S3 has one
        // DMA-capable RMT channel, nothing else claims it, and the render
        // task shares a machine with Wi-Fi.
        ws2812b_config_t sc = {};
        sc.gpio = cfg_.led_pin;
        sc.pixel_count = cfg_.led_count;
        sc.order = WS2812B_ORDER_GRB; // unused: write_raw only
        sc.reset_us = 0;              // driver default (300 us, V5-safe)
        sc.with_dma = true;
        auto *strip = new (std::nothrow) WS2812B(&sc);
        if (strip == nullptr || !strip->ok())
        {
            ESP_LOGE(TAG, "ws2812b init failed (pin %d, count %u)",
                     (int)cfg_.led_pin, (unsigned)cfg_.led_count);
            delete strip;
            deinit();
            return ESP_FAIL;
        }
        strip_ = strip;

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
        if (strip_)
        {
            delete static_cast<WS2812B *>(strip_); // transmits a dark frame
            strip_ = nullptr;
        }
        delete[] map_;
        map_ = nullptr;
        delete[] frame_;
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

        auto *strip = static_cast<WS2812B *>(strip_);

        // White, so all three elements are exercised. It is also the one colour
        // that looks identical whatever byte order the strip uses, which is
        // correct here: this answers "is the pixel alive", and the fixture in
        // fs_seed answers "is the order right".
        for (size_t on = 0; on < cfg_.led_count; ++on)
        {
            memset(frame_, 0, cfg_.led_count * 3);
            bytes_for_order(cfg_.color_order, 255, 255, 255, &frame_[on * 3]);
            strip->write_raw(frame_, cfg_.led_count * 3);
            strip->show(true);
            vTaskDelay(pdMS_TO_TICKS(ms_per_led));
        }

        // Leave the string dark rather than holding the last pixel on: the
        // next thing to touch it is the render task, and a stuck pixel between
        // the two would look like a fault this test had just caused.
        memset(frame_, 0, cfg_.led_count * 3);
        strip->write_raw(frame_, cfg_.led_count * 3);
        strip->show(true);
        return ESP_OK;
    }

    esp_err_t LampMap::fill(uint8_t r, uint8_t g, uint8_t b)
    {
        if (!initialized_)
            return ESP_ERR_INVALID_STATE;

        for (size_t i = 0; i < cfg_.led_count; ++i)
            bytes_for_order(cfg_.color_order, r, g, b, &frame_[i * 3]);
        auto *strip = static_cast<WS2812B *>(strip_);
        esp_err_t err = strip->write_raw(frame_, cfg_.led_count * 3);
        if (err != ESP_OK)
            return err;
        return strip->show(true);
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
        return static_cast<WS2812B *>(strip_)->max_refresh_hz();
    }

    void LampMap::set_override(int16_t led, uint8_t r, uint8_t g, uint8_t b,
                               uint8_t level, bool raw)
    {
        const uint64_t w =
            (uint64_t{1} << 48) | (raw ? (uint64_t{1} << 49) : 0) |
            (uint64_t{level} << 40) | (uint64_t{b} << 32) |
            (uint64_t{g} << 24) | (uint64_t{r} << 16) |
            static_cast<uint16_t>(led);
        override_.store(w, std::memory_order_release);
        ESP_LOGI(TAG, "colour lab: LED %d <- (%u,%u,%u) level %u %s",
                 (int)led, r, g, b, level, raw ? "raw" : "linearised");
    }

    void LampMap::clear_override()
    {
        if (override_.exchange(0, std::memory_order_acq_rel) != 0)
            ESP_LOGI(TAG, "colour lab: override cleared");
    }

    esp_err_t LampMap::render(const uint8_t *levels, size_t n)
    {
        if (!initialized_)
            return ESP_ERR_INVALID_STATE;
        if (!levels)
            return ESP_ERR_INVALID_ARG;

        // Build the whole strip, dark by default, so an LED with no channel
        // mapped to it is driven off rather than left showing whatever it had.
        memset(frame_, 0, cfg_.led_count * 3);

        const size_t count = n < cfg_.channel_count ? n : cfg_.channel_count;
        for (size_t ch = 0; ch < count; ++ch)
        {
            const LampMapEntry &e = map_[ch];
            if (e.led_index < 0 || static_cast<size_t>(e.led_index) >= cfg_.led_count)
                continue;
            const uint8_t v = levels[ch];
            // Wire order decided HERE (bytes_for_order, host-tested), and
            // nowhere else: the driver's write_raw transmits these bytes
            // verbatim, MSB-first — proven per-version on the bench.
            bytes_for_order(cfg_.color_order, scale8(e.r, v), scale8(e.g, v),
                            scale8(e.b, v), &frame_[e.led_index * 3]);
        }

        // The colour lab wins over everything (bench harness, cleared on
        // request or reboot): one coherent load, then exactly the same
        // packing the mapped path uses, so what it proves transfers.
        const uint64_t ov = override_.load(std::memory_order_acquire);
        if (ov & (uint64_t{1} << 48))
        {
            const uint16_t led = static_cast<uint16_t>(ov & 0xFFFF);
            if (led < cfg_.led_count)
            {
                const bool raw = (ov >> 49) & 1;
                const uint8_t lv = (ov >> 40) & 0xFF;
                uint8_t r = (ov >> 16) & 0xFF, g = (ov >> 24) & 0xFF,
                        b = (ov >> 32) & 0xFF;
                if (!raw)
                {
                    r = tint_lut_[r];
                    g = tint_lut_[g];
                    b = tint_lut_[b];
                }
                bytes_for_order(cfg_.color_order, scale8(r, lv),
                                scale8(g, lv), scale8(b, lv),
                                &frame_[led * 3]);
            }
        }

        // ONE transmit per refresh (FR-LED-6). write_raw copies into the
        // driver's caller half; show(false) waits out any in-flight frame,
        // copies to the transmit half, and queues — so back-to-back renders
        // can never tear a frame on the wire.
        auto *strip = static_cast<WS2812B *>(strip_);
        const esp_err_t werr = strip->write_raw(frame_, cfg_.led_count * 3);
        if (werr != ESP_OK)
            return werr;
        return strip->show(false);
    }
} // namespace ooe::pinled
