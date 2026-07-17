/**
 * @file lamp_scan.cpp
 * @brief 74HC161 + 74HC251 scan driver implementation (bit-bang).
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "lamp_scan.h"

#include "esp_rom_sys.h" // esp_rom_delay_us
#include "esp_log.h"

namespace ooe::pinled
{
    static const char *TAG = "lamp_scan";

    esp_err_t LampScan::init(const LampScanConfig &cfg)
    {
        if (cfg.clk_pin == GPIO_NUM_NC || cfg.mr_pin == GPIO_NUM_NC)
            return ESP_ERR_INVALID_ARG;
        if (cfg.num_modules == 0 || cfg.num_modules > LAMPSCAN_MAX_MODULES)
            return ESP_ERR_INVALID_ARG;
        if (cfg.channels_per_module != 8 && cfg.channels_per_module != 16)
            return ESP_ERR_INVALID_ARG;

        cfg_ = cfg;

        gpio_reset_pin(cfg_.clk_pin);
        gpio_set_direction(cfg_.clk_pin, GPIO_MODE_OUTPUT);
        gpio_set_level(cfg_.clk_pin, 0);

        gpio_reset_pin(cfg_.mr_pin);
        gpio_set_direction(cfg_.mr_pin, GPIO_MODE_OUTPUT);
        gpio_set_level(cfg_.mr_pin, 1); // /MR idle high (not clearing)

        for (size_t m = 0; m < cfg_.num_modules; ++m)
        {
            const gpio_num_t d = cfg_.data_pins[m];
            if (d == GPIO_NUM_NC)
            {
                ESP_LOGE(TAG, "module %u has no DATA_IN pin", (unsigned)m);
                return ESP_ERR_INVALID_ARG;
            }
            gpio_reset_pin(d);
            gpio_set_direction(d, GPIO_MODE_INPUT);
        }

        initialized_ = true;
        reset_counter();
        ESP_LOGI(TAG, "init: %u module(s) x %u ch, active_%s",
                 (unsigned)cfg_.num_modules, (unsigned)cfg_.channels_per_module,
                 cfg_.active_low ? "low" : "high");
        return ESP_OK;
    }

    void LampScan::reset_counter()
    {
        // 74HC161 /MR is asynchronous: a low pulse zeroes Q0..Q3 immediately.
        gpio_set_level(cfg_.mr_pin, 0);
        esp_rom_delay_us(1);
        gpio_set_level(cfg_.mr_pin, 1);
    }

    inline void LampScan::clock_pulse()
    {
        gpio_set_level(cfg_.clk_pin, 1);
        gpio_set_level(cfg_.clk_pin, 0);
    }

    inline bool LampScan::sample(size_t module)
    {
        const int raw = gpio_get_level(cfg_.data_pins[module]);
        return cfg_.active_low ? (raw == 0) : (raw != 0);
    }

    esp_err_t LampScan::read_frame(bool *out, size_t n)
    {
        if (!initialized_)
            return ESP_ERR_INVALID_STATE;
        const size_t total = total_channels();
        if (!out || n < total)
            return ESP_ERR_INVALID_ARG;

        reset_counter(); // frame starts at channel 0

        for (size_t ch = 0; ch < cfg_.channels_per_module; ++ch)
        {
            if (cfg_.settle_ns)
                esp_rom_delay_us(1); // coarse settle; sub-us not needed at HC speeds

            for (size_t m = 0; m < cfg_.num_modules; ++m)
                out[m * cfg_.channels_per_module + ch] = sample(m);

            clock_pulse(); // advance all counters in lockstep
        }

        return ESP_OK;
    }
} // namespace ooe::pinled
