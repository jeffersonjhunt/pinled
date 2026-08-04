/**
 * @file main.cpp
 * @brief pinled v2 application entry and task wiring.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "main.h"

#include <cstring>
#include <memory>
#include <new>

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
        sc.data_pin = cfg_.data_pin;
        sc.mr_pin = cfg_.mr_pin;
        sc.spi_hz = cfg_.spi_hz;
        sc.spi_mode = cfg_.spi_mode;
        sc.mr_from_cs = cfg_.mr_from_cs;
        sc.arm_clock = cfg_.arm_clock;
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

        // Heap-allocated: MAX_CHANNELS-sized arrays overflow the main task stack.
        std::unique_ptr<ChannelProfile[]> profiles(new (std::nothrow) ChannelProfile[num_channels_]());
        std::unique_ptr<FilamentParams[]> params(new (std::nothrow) FilamentParams[num_channels_]());
        if (!profiles || !params)
        {
            ESP_LOGE(TAG, "auto-profiling skipped: out of memory");
            return;
        }
        if (profiler_.classify(profiles.get(), params.get(), num_channels_) == ESP_OK)
        {
            for (size_t ch = 0; ch < num_channels_; ++ch)
                filament_.set_params(ch, params[ch]);
            ESP_LOGI(TAG, "auto-profiling applied");
        }
    }

    esp_err_t Main::start_tasks()
    {
#if CONFIG_PINLED_SCAN_STEP_MS > 0 || CONFIG_PINLED_SCAN_HOLD_CH >= 0 || defined(CONFIG_PINLED_SPI_SWEEP)
        // The bring-up walk/hold/sweep in run() owns the scan hardware; a
        // concurrent scan_task would fight it.
        ESP_LOGW(TAG, "bring-up scan mode: scan_task not started, LEDs will not update");
#else
        if (xTaskCreatePinnedToCore(scan_task, "pinled_scan", 4096, this,
                                    configMAX_PRIORITIES - 2, &scan_task_, 1) != pdPASS)
            return ESP_FAIL;
#endif
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

#ifdef CONFIG_PINLED_SCAN_DEBUG
                for (size_t ch = 0; ch < self->num_channels_; ++ch)
                {
                    self->raw_snapshot_[ch] = frame[ch];
                    (frame[ch] ? self->seen_high_ : self->seen_low_)[ch] = 1;
                }
#endif
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

#ifdef CONFIG_PINLED_SCAN_DEBUG
    // Bring-up aid. "raw" is the pre-filament frame straight off the bus; "tog"
    // marks channels that have been observed BOTH high and low since boot. A
    // channel stuck at '-' is either unconnected, unpopulated, or held by the
    // bus bias -- it is never a channel that is merely idle, because an idle
    // lamp still reads a stable 0 and gets its low seen on the first frame.
    void Main::log_scan_debug()
    {
        constexpr size_t kMax = LampScan::MAX_CHANNELS;
        char raw[kMax + kMax / 8 + 2];
        char tog[kMax + kMax / 8 + 2];
        size_t r = 0, t = 0;

        for (size_t ch = 0; ch < num_channels_; ++ch)
        {
            if (ch && (ch % 8) == 0)
            {
                raw[r++] = ' ';
                tog[t++] = ' ';
            }
            raw[r++] = raw_snapshot_[ch] ? '1' : '0';
            tog[t++] = (seen_high_[ch] && seen_low_[ch]) ? 'T' : '-';
        }
        raw[r] = '\0';
        tog[t] = '\0';

        ESP_LOGI(TAG, "raw [%s]  tog [%s]", raw, tog);
    }
#endif

#ifdef CONFIG_PINLED_SPI_SWEEP
    // Step the chain clock and report, per rate, whether repeated reads agree.
    // Hold one test input down: a good rate gives a fully stable burst whose
    // union of observed bits is exactly that one channel. Instability, or extra
    // bits in the union, is the ceiling.
    void Main::spi_sweep()
    {
        static const int kRates[] = {
            1000000, 2000000, 4000000, 6000000, 8000000,
            10000000, 13000000, 16000000, 20000000, 26000000, 40000000};
        constexpr int kFramesPerRate = 256;
        constexpr int kDiscard = 8; // let the new divider settle

        bool frame[LampScan::MAX_CHANNELS];
        char bits[LampScan::MAX_CHANNELS + 2];

        for (;;)
        {
            ESP_LOGI(TAG, "=== chain clock sweep: %u channels, hold one input down ===",
                     (unsigned)num_channels_);

            for (int r = 0; r < (int)(sizeof(kRates) / sizeof(kRates[0])); ++r)
            {
                if (scan_.set_clock(kRates[r]) != ESP_OK)
                {
                    ESP_LOGW(TAG, "%9d Hz: rejected by driver", kRates[r]);
                    continue;
                }

                for (int i = 0; i < kDiscard; ++i)
                    scan_.read_frame(frame, num_channels_);

                uint64_t first = 0, any = 0, all = ~0ULL;
                int stable = 0, reads = 0;
                for (int i = 0; i < kFramesPerRate; ++i)
                {
                    if (scan_.read_frame(frame, num_channels_) != ESP_OK)
                        continue;
                    uint64_t m = 0;
                    for (size_t ch = 0; ch < num_channels_ && ch < 64; ++ch)
                        if (frame[ch])
                            m |= 1ULL << ch;
                    if (reads == 0)
                        first = m;
                    any |= m;
                    all &= m;
                    if (m == first)
                        ++stable;
                    ++reads;
                }

                size_t b = 0;
                for (size_t ch = 0; ch < num_channels_ && ch < 64; ++ch)
                    bits[b++] = (first >> ch) & 1 ? '1' : '0';
                bits[b] = '\0';

                ESP_LOGI(TAG, "%9d Hz (actual %8d): stable %3d/%3d  [%s]  union=0x%llx  common=0x%llx",
                         kRates[r], scan_.actual_hz(), stable, reads, bits,
                         (unsigned long long)any,
                         (unsigned long long)(reads ? all : 0));
            }

            ESP_LOGI(TAG, "=== sweep pass complete ===");
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
#endif

#if CONFIG_PINLED_SCAN_HOLD_CH >= 0
    void Main::hold_channel()
    {
        const unsigned ch = CONFIG_PINLED_SCAN_HOLD_CH;

        scan_.reset_counter();
        for (unsigned i = 0; i < ch; ++i)
            scan_.step_counter();

        ESP_LOGI(TAG, "counter parked on channel %u; Q3..Q0 = %u%u%u%u, '151 C/B/A = %u%u%u",
                 ch, (ch >> 3) & 1, (ch >> 2) & 1, (ch >> 1) & 1, ch & 1,
                 (ch >> 2) & 1, (ch >> 1) & 1, ch & 1);
        ESP_LOGI(TAG, "address lines are now static -- measure D%u -> Y -> MCU DATA pin", ch);

        for (;;)
        {
            ESP_LOGI(TAG, "hold ch %u  DATA=%d", ch, scan_.sample_now(0) ? 1 : 0);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
#endif

#if CONFIG_PINLED_SCAN_STEP_MS > 0
    void Main::step_walk()
    {
        ESP_LOGI(TAG, "slow-step scan: %d ms/channel, %u channels",
                 CONFIG_PINLED_SCAN_STEP_MS, (unsigned)num_channels_);
        ESP_LOGI(TAG, "watch the '161 Q outputs; 'expect' is the count they should show");

        for (;;)
        {
            scan_.reset_counter();
            ESP_LOGI(TAG, "--- /MR pulsed, counter should now read 0000 ---");

            for (size_t ch = 0; ch < num_channels_; ++ch)
            {
                vTaskDelay(pdMS_TO_TICKS(CONFIG_PINLED_SCAN_STEP_MS));
                const bool v = scan_.sample_now(0);
                ESP_LOGI(TAG, "count %2u  expect Q3..Q0 = %u%u%u%u  DATA=%d",
                         (unsigned)ch,
                         (unsigned)((ch >> 3) & 1), (unsigned)((ch >> 2) & 1),
                         (unsigned)((ch >> 1) & 1), (unsigned)(ch & 1),
                         v ? 1 : 0);
                scan_.step_counter();
            }
        }
    }
#endif

    void Main::run()
    {
#ifdef CONFIG_PINLED_SPI_SWEEP
        spi_sweep(); // never returns
#elif CONFIG_PINLED_SCAN_HOLD_CH >= 0
        hold_channel(); // never returns; takes precedence over the walk
#elif CONFIG_PINLED_SCAN_STEP_MS > 0
        step_walk(); // never returns
#endif
#ifdef CONFIG_PINLED_SCAN_DEBUG
        // Tasks do the work; report the raw bus so bring-up has something to
        // look at that does not depend on the LED path working.
        for (;;)
        {
            log_scan_debug();
            vTaskDelay(pdMS_TO_TICKS(500));
        }
#else
        // Tasks do the work; keep app_main alive and emit a slow heartbeat.
        for (;;)
        {
            ESP_LOGD(TAG, "tick");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
#endif
    }
} // namespace ooe::pinled
