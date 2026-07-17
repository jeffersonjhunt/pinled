/**
 * @file main.cpp
 * @brief pinled v2 application entry and task wiring.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "main.h"

#include <cstring>

#include "esp_rom_sys.h"

namespace ooe::pinled
{
    static const char *TAG = "pinled-main";

    Main *pinled_main;

    extern "C" void app_main()
    {
        pinled_main = new Main();
        ESP_ERROR_CHECK(pinled_main->init());
        pinled_main->run();
    }

    void Main::version()
    {
        ESP_LOGI(TAG, "%s version: %i.%i.%i", PROJECT_NAME,
                 PROJECT_VERSION_MAJOR, PROJECT_VERSION_MINOR, PROJECT_VERSION_PATCH);
    }

    esp_err_t Main::init()
    {
        esp_log_level_set("*", ESP_LOG_DEBUG);
        version();
        ESP_LOGI(TAG, "Initializing...");

        // 1. Configuration (NVS profile or Kconfig defaults).
        ESP_ERROR_CHECK(store_.load(cfg_));
        num_channels_ = cfg_.total_channels();
        if (num_channels_ > LampScan::MAX_CHANNELS)
        {
            ESP_LOGE(TAG, "too many channels: %u", (unsigned)num_channels_);
            return ESP_ERR_INVALID_SIZE;
        }

        // 2. Scan driver.
        LampScanConfig sc{};
        sc.clk_pin = cfg_.clk_pin;
        sc.mr_pin = cfg_.mr_pin;
        for (size_t m = 0; m < cfg_.num_modules; ++m)
            sc.data_pins[m] = cfg_.data_pins[m];
        sc.num_modules = cfg_.num_modules;
        sc.channels_per_module = cfg_.channels_per_module;
        sc.active_low = cfg_.active_low;
        ESP_ERROR_CHECK(scan_.init(sc));

        // 3. Filament bank.
        ESP_ERROR_CHECK(filament_.init(num_channels_, cfg_.sample_rate_hz));
        filament_.set_gamma(cfg_.gamma);
        FilamentParams fp{};
        fp.attack_ms = cfg_.attack_ms;
        fp.decay_ms = cfg_.decay_ms;
        ESP_ERROR_CHECK(filament_.set_params_all(fp));

        // 4. Profiler (used at boot, then idle).
        ESP_ERROR_CHECK(profiler_.init(num_channels_, cfg_.sample_rate_hz));

        // 5. LED map + string.
        LampMapConfig mc{};
        mc.led_pin = cfg_.led_pin;
        mc.led_count = cfg_.led_count;
        mc.channel_count = num_channels_;
        ESP_ERROR_CHECK(map_.init(mc));

        // 6. Boot-time auto-profiling pass.
        profile_boot();

        ESP_ERROR_CHECK(start_tasks());
        ESP_LOGI(TAG, "Initializing complete. %u channels, %u LEDs.",
                 (unsigned)num_channels_, (unsigned)cfg_.led_count);
        return ESP_OK;
    }

    void Main::profile_boot()
    {
        ESP_LOGI(TAG, "auto-profiling (%u channels)...", (unsigned)num_channels_);
        bool frame[LampScan::MAX_CHANNELS];

        profiler_.arm();
        // Observe a window long enough to span several AC/matrix periods.
        const int kFrames = 512;
        for (int i = 0; i < kFrames; ++i)
        {
            if (scan_.read_frame(frame, num_channels_) == ESP_OK)
                profiler_.observe(frame, num_channels_);
        }

        ChannelProfile profiles[LampScan::MAX_CHANNELS]{};
        FilamentParams params[LampScan::MAX_CHANNELS]{};
        if (profiler_.classify(profiles, params, num_channels_) == ESP_OK)
        {
            for (size_t ch = 0; ch < num_channels_; ++ch)
                filament_.set_params(ch, params[ch]);
            ESP_LOGI(TAG, "auto-profiling applied");
        }
    }

    esp_err_t Main::start_tasks()
    {
        if (xTaskCreatePinnedToCore(scan_task, "pinled_scan", 4096, this,
                                    configMAX_PRIORITIES - 2, &scan_task_, 1) != pdPASS)
            return ESP_FAIL;
        if (xTaskCreatePinnedToCore(render_task, "pinled_render", 4096, this,
                                    5, &render_task_, 0) != pdPASS)
            return ESP_FAIL;
        return ESP_OK;
    }

    // High-rate: sample every channel, feed the filament integrators, publish
    // the 0..255 levels for the renderer. This is the clock-domain crossing.
    void Main::scan_task(void *arg)
    {
        Main *self = static_cast<Main *>(arg);
        bool frame[LampScan::MAX_CHANNELS];
        uint32_t ticks = 0;

        for (;;)
        {
            if (self->scan_.read_frame(frame, self->num_channels_) == ESP_OK)
            {
                for (size_t ch = 0; ch < self->num_channels_; ++ch)
                    self->filament_.update(ch, frame[ch]);
                self->filament_.snapshot(self->levels_, self->num_channels_);
            }

            // Keep the task WDT fed during bring-up. Replace with a hardware
            // timer / dedic_gpio pacing loop to hit an exact sample rate.
            if ((++ticks & 0x3F) == 0)
                vTaskDelay(1);
        }
    }

    // Frame-rate: render the published levels to the LED string.
    void Main::render_task(void *arg)
    {
        Main *self = static_cast<Main *>(arg);
        const TickType_t period = pdMS_TO_TICKS(1000UL / (self->cfg_.refresh_hz ? self->cfg_.refresh_hz : 60));
        TickType_t last = xTaskGetTickCount();

        for (;;)
        {
            self->map_.render(self->levels_, self->num_channels_);
            vTaskDelayUntil(&last, period > 0 ? period : 1);
        }
    }

    void Main::run()
    {
        // Tasks do the work; keep app_main alive and emit a slow heartbeat.
        for (;;)
        {
            ESP_LOGD(TAG, "tick");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
} // namespace ooe::pinled
