/**
 * @file main.cpp
 * @brief pinled v2 application entry and task wiring.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "main.h"

#include <cstring>
#include <memory>
#include <new>

#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_check.h"
#include "esp_timer.h"

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

    void Main::log_build_options()
    {
        // Every option here changes behaviour, and none of them is visible in
        // the source: `sdkconfig` is gitignored and lives only on the build
        // host, so anyone reading a capture — including me, reading one from
        // another machine — cannot otherwise tell which build produced it.
        //
        // That is not hypothetical. CONFIG_PINLED_SCAN_DEBUG selects a
        // different run loop, and being unable to see it set meant two rounds
        // of latency figures measured on a code path the design comment did
        // not describe. One line at boot would have caught it immediately.
        // Each #ifdef reduced to a bool once, so the log line and the warning
        // below cannot disagree about what is enabled.
#ifdef CONFIG_PINLED_SCAN_DEBUG
        constexpr bool kScanDebug = true;
#else
        constexpr bool kScanDebug = false;
#endif
#ifdef CONFIG_PINLED_PROFILE_REARM_BUTTON
        constexpr bool kRearmButton = true;
#else
        constexpr bool kRearmButton = false;
#endif
#ifdef CONFIG_PINLED_STARTUP_WALK
        constexpr bool kWalk = true;
#else
        constexpr bool kWalk = false;
#endif
#ifdef CONFIG_PINLED_SPI_SWEEP
        constexpr bool kSweep = true;
#else
        constexpr bool kSweep = false;
#endif
#ifdef CONFIG_PINLED_STORE_SELFTEST
        constexpr bool kSelftest = true;
#else
        constexpr bool kSelftest = false;
#endif
#ifdef CONFIG_PINLED_LED_LOAD_TEST
        constexpr bool kLoadTest = true;
#else
        constexpr bool kLoadTest = false;
#endif
#ifdef CONFIG_PINLED_SEED_FS
        constexpr bool kSeedFs = true;
#else
        constexpr bool kSeedFs = false;
#endif
        auto yn = [](bool b) { return b ? "on" : "off"; };

        ESP_LOGI(TAG, "build: profiler window %d ms, re-arm button %s, "
                      "rescue GPIO %d @ %d ms, startup walk %s",
                 CONFIG_PINLED_PROFILE_WINDOW_MS, yn(kRearmButton),
                 CONFIG_PINLED_RESCUE_GPIO, CONFIG_PINLED_RESCUE_HOLD_MS, yn(kWalk));

        // Every option named even when it is off. A banner that listed only
        // what was enabled would read identically to one from a build where
        // the option did not exist at all.
        ESP_LOGI(TAG, "build: diagnostics scan_debug=%s step_ms=%d hold_ch=%d "
                      "spi_sweep=%s selftest=%s load_test=%s seed_fs=%s",
                 yn(kScanDebug), CONFIG_PINLED_SCAN_STEP_MS, CONFIG_PINLED_SCAN_HOLD_CH,
                 yn(kSweep), yn(kSelftest), yn(kLoadTest), yn(kSeedFs));

        // Loud, because each of these changes what the board *is*: the first
        // three take the scan hardware away from scan_task, and seed_fs
        // overwrites the stored configuration on every flash.
        if (CONFIG_PINLED_SCAN_STEP_MS > 0 || CONFIG_PINLED_SCAN_HOLD_CH >= 0 ||
            kSweep || kSeedFs)
            ESP_LOGW(TAG, "build: a bring-up option is active — this is NOT a "
                          "normal-operation build");
    }

    esp_err_t Main::init()
    {
        esp_log_level_set("*", ESP_LOG_DEBUG);
        version();
        log_build_options();
        ESP_LOGI(TAG, "Initializing...");

        // 0. The indicator, before anything that can fail. It needs nothing
        //    but a GPIO and an RMT channel, and every fault below it has
        //    somewhere to go only because it is already running (FR-IND-1).
        start_indicator();

#ifdef CONFIG_PINLED_STORE_SELFTEST
        // Before anything is loaded, so it cannot disturb a real configuration.
        store_.selftest();
#endif

        // 1. Configuration: the stored documents, or Kconfig defaults for
        //    whatever is absent or unusable (FR-CFG-4). This also resolves the
        //    per-channel records, so there is one place a configuration is
        //    decided and it is not here.
        ESP_ERROR_CHECK(store_.load(cfg_, channels_, LampScan::MAX_CHANNELS, &num_channels_));
        if (num_channels_ == 0 || num_channels_ > LampScan::MAX_CHANNELS)
        {
            ESP_LOGE(TAG, "bad channel count: %u", (unsigned)num_channels_);
            return ESP_ERR_INVALID_SIZE;
        }

        // The store reports its troubles in state() rather than as an error,
        // because both are survivable and it falls back to defaults. That is
        // exactly the shape FR-IND-4 wants: the device keeps doing what it
        // can, and red means "not doing what you asked" rather than "dead".
        {
            const StoreState &st = store_.state();
            status_.set_fault(FaultClass::STORAGE, !st.mounted);
            status_.set_fault(FaultClass::CONFIG,
                              st.install_rejected || st.profile_rejected);
        }

        // The pixel started at step 0 on the Kconfig default because it had
        // to exist before the store could fail; the stored setting takes over
        // the moment it is known (FR-IND-5). 0 here is off, and stays off.
        status_.set_brightness(cfg_.status_brightness);
#if CONFIG_PINLED_STATUS_GPIO >= 0
        // Logged only when it changes something: the takeover is otherwise
        // invisible in a log, and the bench checklist needs a line to look for.
        if (cfg_.status_brightness != CONFIG_PINLED_STATUS_BRIGHTNESS)
            ESP_LOGI(TAG, "status pixel brightness %u/255 (stored, overrides build default %u)",
                     (unsigned)cfg_.status_brightness,
                     (unsigned)CONFIG_PINLED_STATUS_BRIGHTNESS);
#endif

        // 2. Scan driver.
        LampScanConfig sc{};
        sc.clk_pin = static_cast<gpio_num_t>(cfg_.clk_pin);
        sc.data_pin = static_cast<gpio_num_t>(cfg_.data_pin);
        sc.pl_pin = static_cast<gpio_num_t>(cfg_.pl_pin);
        sc.spi_hz = cfg_.spi_hz;
        sc.spi_mode = cfg_.spi_mode;
        sc.pl_from_cs = cfg_.pl_from_cs;
        sc.num_modules = cfg_.num_modules;
        sc.channels_per_module = cfg_.channels_per_module;
        sc.active_low = cfg_.active_low;
        ESP_ERROR_CHECK(scan_.init(sc));

        // 3. Boot feasibility check (FR-SCAN-9). Config is not trusted: measure
        //    what the hardware actually does and derive Fs from that, so every
        //    downstream tau is computed against reality.
        ESP_ERROR_CHECK(measure_and_clamp_fs());

        // 4. Filament bank, seeded with the MEASURED rate, not the configured one.
        ESP_ERROR_CHECK(filament_.init(num_channels_, fs_actual_));
        filament_.set_gamma(cfg_.gamma);

        // 5. Profiler. The window is milliseconds and the frame count comes
        //    from the measured Fs, so a slower chain observes for the same
        //    length of time rather than the same number of frames (FR-PROF-5).
        ESP_ERROR_CHECK(profiler_.init(num_channels_, fs_actual_,
                                       CONFIG_PINLED_PROFILE_WINDOW_MS));
        profile_window_frames_ = profiler_.window_frames();

        // 6. LED map + string.
        LampMapConfig mc{};
        mc.led_pin = static_cast<gpio_num_t>(cfg_.led_pin);
        mc.led_count = cfg_.led_count;
        mc.channel_count = num_channels_;
        mc.color_order = cfg_.color_order;
        ESP_ERROR_CHECK(map_.init(mc));
        clamp_refresh(); // FR-LED-8

#ifdef CONFIG_PINLED_LED_LOAD_TEST
        // Bring-up only: hold the string at rising brightness so a meter in
        // series can be read at each step. Before the render task exists, so
        // nothing else is touching the strip.
        {
            const uint8_t steps[] = {64, 128, 191, 255};
            for (uint8_t v : steps)
            {
                ESP_LOGW(TAG, "load test: %u%% white on %u LEDs — read the meter now",
                         (unsigned)((v * 100u + 127u) / 255u), (unsigned)cfg_.led_count);
                map_.fill(v, v, v);
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            map_.fill(0, 0, 0);
            ESP_LOGW(TAG, "load test: done, string dark");
        }
#endif

#ifdef CONFIG_PINLED_STARTUP_WALK
        // Before anything is sensed or mapped: prove the string itself works.
        // Deliberately here rather than after apply_channel_config(), so a
        // playfield with half its channels unmapped still lights every pixel.
        map_.walk(CONFIG_PINLED_STARTUP_WALK_MS);
#endif

        // 7. Configuration becomes running behaviour. Must follow both
        //    filament_.init() and map_.init(), since it writes into each.
        apply_channel_config();

        // 8. Tasks, then the boot profiling pass — in that order, which is a
        //    change from the first cut and the point of this whole path.
        //    Profiling now happens by having the scan task hand its frames to
        //    the profiler, because at runtime that task owns the chain and
        //    nothing else may read it. Boot going through the same route is
        //    what stops the two from drifting: there is one way a
        //    classification is produced, and the button and the API use it.
        main_task_ = xTaskGetCurrentTaskHandle();
        ESP_ERROR_CHECK(start_tasks());
        ESP_ERROR_CHECK(start_pacing());
        profile_and_wait("boot");

        ESP_LOGI(TAG, "Initializing complete. %u channels, %u LEDs, Fs %.0f Hz, %u Hz refresh.",
                 (unsigned)num_channels_, (unsigned)cfg_.led_count,
                 fs_actual_, (unsigned)cfg_.refresh_hz);

        // 9. Network and API, last. Nothing above depends on it.
        start_network();
        indicator_settle();

        // M4 puts Wi-Fi, lwIP and httpd on top of whatever is left here, and
        // nothing has ever measured it. The minimum is the useful half: the
        // store decodes both documents into heap and frees them again, so the
        // gap between the two numbers is that transient peak rather than a
        // guess at it. The shipping board has no PSRAM to fall back on (NFR-8).
        ESP_LOGI(TAG, "heap: %u bytes free, %u minimum since boot",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)esp_get_minimum_free_heap_size());
        return ESP_OK;
    }

    void Main::start_network()
    {
        // The button first, and before any of the ways this function gives up.
        // It is not a network feature: it is the way back in on a board that
        // joined nothing (FR-UI-7) and the way to re-profile a machine on a
        // bench with no router at all (FR-PROF-2). Starting it after the early
        // returns below made it available only when it was least needed.
        start_button();

        // Every failure here is logged and survived. A board that cannot join
        // a network still senses lamps and still drives LEDs, and making boot
        // conditional on Wi-Fi would turn a router reboot into a dark
        // playfield.
        if (net_.start() != ESP_OK)
        {
            ESP_LOGE(TAG, "network did not start; API unavailable");
            return;
        }
        if (!net_.up())
        {
            ESP_LOGW(TAG, "no address; API unavailable this boot");
            return;
        }
        live_.levels = levels_;
        live_.channels = channels_;
        live_.classes = classes_;
        live_.count = num_channels_;

        if (api_.start(store_, cfg_, channels_, num_channels_, net_, live_, this) != ESP_OK)
        {
            ESP_LOGE(TAG, "API did not start");
            status_.set_fault(FaultClass::INTERNAL, true);
        }
    }

    void Main::start_indicator()
    {
#if CONFIG_PINLED_STATUS_GPIO >= 0
        IndicatorConfig ic{};
        ic.pin = static_cast<gpio_num_t>(CONFIG_PINLED_STATUS_GPIO);
        ic.power_pin = CONFIG_PINLED_STATUS_POWER_GPIO >= 0
                           ? static_cast<gpio_num_t>(CONFIG_PINLED_STATUS_POWER_GPIO)
                           : GPIO_NUM_NC;
        ic.brightness = static_cast<uint8_t>(CONFIG_PINLED_STATUS_BRIGHTNESS);
        ic.hold_erase_ms = CONFIG_PINLED_RESCUE_HOLD_MS;
        if (status_.init(ic) != ESP_OK)
            ESP_LOGW(TAG, "status pixel unavailable; the board still runs, it "
                          "just cannot say so");
#else
        ESP_LOGW(TAG, "status pixel not fitted (PINLED_STATUS_GPIO = -1)");
#endif
    }

    void Main::indicator_settle()
    {
        // Three outcomes, and start_network() survives all of them, so the
        // resting state is read back from what happened rather than assumed
        // from what was attempted.
        if (net_.awaiting_provisioning())
            status_.set_state(IndicatorState::SOFTAP);
        else if (net_.up())
            status_.set_state(IndicatorState::RUNNING);
        else
            status_.set_state(IndicatorState::RUNNING_OFFLINE);
    }

    void Main::start_button()
    {
        // One pin, two jobs, split by how long it is held: a long hold erases
        // the network (FR-UI-7), a short press re-runs the classifier
        // (FR-PROF-2). The callback runs on the button's own task, so it does
        // nothing but post the request — the observation window belongs to the
        // scan task and the classification to the main task.
        // Live press reporting, independent of what the press turns out to
        // mean (FR-IND-7/8). Wired even when the re-arm is compiled out: the
        // long hold still exists, and "is this button doing anything" is
        // exactly the question a person asks while holding it.
        ButtonFeedback fb{};
        fb.arg = this;
        fb.accepted = [](void *self) { static_cast<Main *>(self)->status_.note_press(); };
        fb.held = [](void *self, uint32_t ms) {
            static_cast<Main *>(self)->status_.set_hold(ms);
        };

#ifdef CONFIG_PINLED_PROFILE_REARM_BUTTON
        auto on_short_press = [](void *self) {
            if (!static_cast<Main *>(self)->rearm_profiler())
                ESP_LOGI(TAG, "button re-arm ignored: a pass is already running");
        };
        rescue_button_start(CONFIG_PINLED_RESCUE_GPIO, CONFIG_PINLED_RESCUE_HOLD_MS,
                            on_short_press, this, &fb);
#else
        rescue_button_start(CONFIG_PINLED_RESCUE_GPIO, CONFIG_PINLED_RESCUE_HOLD_MS,
                            nullptr, nullptr, &fb);
#endif
    }

    void Main::apply_channel_config()
    {
        for (size_t ch = 0; ch < num_channels_; ++ch)
        {
            const ChannelConfig &c = channels_[ch];

            filament_.set_params(ch, params_from_config(c));

            LampMapEntry e{};
            e.led_index = c.led_index;
            e.r = c.r;
            e.g = c.g;
            e.b = c.b;
            map_.set_entry(ch, e);
        }
    }

    bool Main::rearm_profiler()
    {
        if (scan_task_ == nullptr)
        {
            // A bring-up scan mode owns the chain, so nothing is feeding the
            // profiler and nothing ever will on this build.
            ESP_LOGW(TAG, "profiler re-arm refused: no scan task on this build");
            return false;
        }

        // Only an idle profiler accepts a request, and the phase and the pass
        // number advance together. Losing the exchange means someone else got
        // there first, which is a refusal and not an error: the pass they
        // started is the one this caller wanted.
        uint32_t word = profile_word_.load(std::memory_order_acquire);
        for (;;)
        {
            if (profile_phase(word) != ProfilerState::IDLE)
                return false;
            const uint32_t next = profile_pack(ProfilerState::OBSERVING,
                                               profile_pass(word) + 1);
            if (profile_word_.compare_exchange_weak(word, next,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire))
                break;
        }
        profile_started_ms_.store(static_cast<uint32_t>(esp_timer_get_time() / 1000),
                                  std::memory_order_relaxed);

        // One brief white flash. Deliberately NOT the press acknowledgement:
        // a pass only starts if the request was accepted, so using it as the
        // acknowledgement would make a refused re-arm indistinguishable from
        // a button that did nothing (FR-IND-7).
        status_.note_profiling();

        ESP_LOGI(TAG, "auto-profiling: window %u ms (%u frames at %.0f Hz)",
                 (unsigned)CONFIG_PINLED_PROFILE_WINDOW_MS,
                 (unsigned)profile_window_frames_, fs_actual_);
        return true;
    }

    ProfilerStatus Main::profiler_status() const
    {
        ProfilerStatus s{};
        s.state = scan_task_
                      ? profile_phase(profile_word_.load(std::memory_order_relaxed))
                      : ProfilerState::UNAVAILABLE;
        s.window_ms = CONFIG_PINLED_PROFILE_WINDOW_MS;
        s.window_frames = profile_window_frames_;
        // Written by the scan task and read here without synchronisation: a
        // 32-bit aligned load cannot tear on this target, so the worst case is
        // a progress number a frame or two behind. Taking a lock in the path
        // of a 10 kHz task to freshen a browser readout would be the wrong
        // trade by a wide margin.
        s.frames_observed = profiler_.frames();
        s.channels = static_cast<uint32_t>(num_channels_);
        s.passes = profile_passes_.load(std::memory_order_relaxed);
        return s;
    }

    void Main::profile_and_wait(const char *reason)
    {
        if (!rearm_profiler())
        {
            ESP_LOGW(TAG, "auto-profiling (%s) skipped", reason);
            return;
        }

        // Generous: the window plus the time the scan task needs to notice the
        // request, times three. If this expires something is wrong with the
        // scan, and boot continues rather than hanging — a machine that cannot
        // classify should still light its lamps with the configured defaults.
        const uint32_t budget_ms = 3 * CONFIG_PINLED_PROFILE_WINDOW_MS + 500;
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(budget_ms);

        while (profile_phase(profile_word_.load(std::memory_order_acquire)) !=
               ProfilerState::CLASSIFYING)
        {
            const TickType_t now = xTaskGetTickCount();
            if (static_cast<int32_t>(deadline - now) <= 0)
            {
                ESP_LOGE(TAG, "auto-profiling (%s) did not complete in %u ms; "
                              "continuing with configured defaults",
                         reason, (unsigned)budget_ms);
                // Left running deliberately: the scan task may be mid-window
                // and merely slow, in which case the pass still lands and
                // service_profile() picks it up from the main loop. If it is
                // wedged instead, the same function abandons it once
                // profile_budget_ms() is up, so nothing stays stuck either way.
                return;
            }
            ulTaskNotifyTake(pdTRUE, deadline - now);
        }
        service_profile();
    }

    void Main::service_profile()
    {
        uint32_t word = profile_word_.load(std::memory_order_acquire);

        if (profile_phase(word) == ProfilerState::OBSERVING)
        {
            // A window that has outlived its budget means the scan task has
            // stopped feeding it — a persistently failing read_frame is the
            // way there, since the profiler is only fed from a good frame.
            // Without this the phase would stay OBSERVING for the life of the
            // boot and every later request would be refused, on a device
            // whose real problem is elsewhere and diagnosable.
            const uint32_t elapsed =
                static_cast<uint32_t>(esp_timer_get_time() / 1000) -
                profile_started_ms_.load(std::memory_order_relaxed);
            if (elapsed > profile_budget_ms())
            {
                // Compare-exchange, not a store: the scan task may be
                // completing this very pass. Losing means it did, and its
                // result is the one to keep.
                if (profile_word_.compare_exchange_strong(
                        word, profile_pack(ProfilerState::IDLE, profile_pass(word)),
                        std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    ESP_LOGE(TAG, "auto-profiling abandoned after %u ms: the scan task "
                                  "stopped feeding the profiler (%u of %u frames)",
                             (unsigned)elapsed, (unsigned)profiler_.frames(),
                             (unsigned)profile_window_frames_);
                    return;
                }
            }
        }

        if (profile_phase(word) != ProfilerState::CLASSIFYING)
            return;

        apply_classification();
        profile_passes_.fetch_add(1, std::memory_order_relaxed);
        // A plain store is enough here: CLASSIFYING is this task's phase and
        // nothing else writes the word while it holds it.
        profile_word_.store(profile_pack(ProfilerState::IDLE, profile_pass(word)),
                            std::memory_order_release);
    }

    void Main::apply_classification()
    {
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
            // The classifier's answer is a suggestion, not a verdict. A channel
            // whose class a person locked keeps its own tuning, and explicit
            // per-lamp values survive regardless of the lock (FR-CFG-8,
            // FR-PROF-4) — otherwise a hand-set fade would be silently replaced
            // on every boot and per-lamp presentation would not work.
            size_t locked = 0;
            for (size_t ch = 0; ch < num_channels_; ++ch)
            {
                const ChannelConfig &c = channels_[ch];
                if (!profiler_may_classify(c))
                {
                    ++locked;
                    profiles[ch].locked = true;
                    profiles[ch].klass = c.class_lock;
                }
                filament_.set_params(ch, effective_params(c, params[ch]));
            }
            // Kept for the live monitor: the class is what the UI colours a
            // channel by, and it is otherwise thrown away here.
            for (size_t ch = 0; ch < num_channels_; ++ch)
                classes_[ch] = profiles[ch].klass;

            ESP_LOGI(TAG, "auto-profiling applied (%u of %u channels locked)",
                     (unsigned)locked, (unsigned)num_channels_);
        }
    }

    esp_err_t Main::measure_and_clamp_fs()
    {
        // Free-run the scan and time it. The measurement catches everything the
        // arithmetic would miss: a clock divider that rounded, a slower chain
        // than configured, a driver overhead we did not predict.
        bool frame[LampScan::MAX_CHANNELS];
        constexpr int kWarm = 16;
        constexpr int kMeasure = 256;

        for (int i = 0; i < kWarm; ++i)
            scan_.read_frame(frame, num_channels_);

        const int64_t t0 = esp_timer_get_time();
        for (int i = 0; i < kMeasure; ++i)
        {
            if (scan_.read_frame(frame, num_channels_) != ESP_OK)
            {
                ESP_LOGE(TAG, "scan failed during feasibility measurement");
                return ESP_FAIL;
            }
        }
        const float t_frame_us = static_cast<float>(esp_timer_get_time() - t0) / kMeasure;
        if (t_frame_us <= 0.0f)
            return ESP_FAIL;

        const float fs_free = 1e6f / t_frame_us;
        // 60% ceiling leaves room for the integrator, the renderer and interrupts.
        const float fs_max = fs_free * 0.60f;

        fs_actual_ = cfg_.sample_rate_hz;
        if (fs_actual_ > fs_max)
        {
            ESP_LOGW(TAG, "configured Fs %.0f Hz is not sustainable; clamping to %.0f Hz",
                     cfg_.sample_rate_hz, fs_max);
            fs_actual_ = fs_max;
        }
        if (fs_actual_ < 1.0f)
        {
            ESP_LOGE(TAG, "measured frame time %.1f us leaves no usable sample rate", t_frame_us);
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "frame %.1f us measured -> free-run %.0f Hz, Fs %.0f Hz (%.0f%% duty)",
                 t_frame_us, fs_free, fs_actual_, 100.0f * fs_actual_ / fs_free);
        return ESP_OK;
    }

    void Main::clamp_refresh()
    {
        const uint32_t max_hz = map_.max_refresh_hz();
        if (max_hz == 0)
            return;
        // 70% ceiling: the strip transmit must not monopolize the RMT/I2S path.
        const uint32_t cap = static_cast<uint32_t>(max_hz * 0.70f);
        if (cfg_.refresh_hz > cap)
        {
            ESP_LOGW(TAG, "refresh %u Hz exceeds what %u LEDs allow; clamping to %u Hz",
                     (unsigned)cfg_.refresh_hz, (unsigned)cfg_.led_count, (unsigned)cap);
            cfg_.refresh_hz = cap;
        }
    }

    // Paces one frame per alarm. Uniform dt is what the filament integrator
    // assumes, so this is a fixed tick rather than a burst-then-sleep scheme.
    static bool IRAM_ATTR on_sample_alarm(gptimer_handle_t, const gptimer_alarm_event_data_t *,
                                          void *arg)
    {
        BaseType_t hpw = pdFALSE;
        vTaskNotifyGiveFromISR(static_cast<TaskHandle_t>(arg), &hpw);
        return hpw == pdTRUE;
    }

    esp_err_t Main::start_pacing()
    {
        if (scan_task_ == nullptr)
            return ESP_OK; // a bring-up mode owns the scan hardware

        gptimer_config_t tc{};
        tc.clk_src = GPTIMER_CLK_SRC_DEFAULT;
        tc.direction = GPTIMER_COUNT_UP;
        tc.resolution_hz = 1000000; // 1 us tick
        ESP_RETURN_ON_ERROR(gptimer_new_timer(&tc, &sample_timer_), TAG, "gptimer_new_timer");

        gptimer_event_callbacks_t cbs{};
        cbs.on_alarm = on_sample_alarm;
        ESP_RETURN_ON_ERROR(gptimer_register_event_callbacks(sample_timer_, &cbs, scan_task_),
                            TAG, "gptimer callbacks");

        gptimer_alarm_config_t ac{};
        ac.alarm_count = static_cast<uint64_t>(1e6f / fs_actual_);
        ac.reload_count = 0;
        ac.flags.auto_reload_on_alarm = true;
        ESP_RETURN_ON_ERROR(gptimer_set_alarm_action(sample_timer_, &ac), TAG, "gptimer alarm");
        ESP_RETURN_ON_ERROR(gptimer_enable(sample_timer_), TAG, "gptimer_enable");
        ESP_RETURN_ON_ERROR(gptimer_start(sample_timer_), TAG, "gptimer_start");

        ESP_LOGI(TAG, "pacing at %.0f Hz (%llu us period)", fs_actual_,
                 (unsigned long long)ac.alarm_count);
        return ESP_OK;
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

        for (;;)
        {
            // One frame per pacing tick (FR-SCAN-8). The take clears the count,
            // so a value above 1 means alarms fired while we were still busy --
            // i.e. we are not keeping up.
            const uint32_t pending = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            if (pending == 0)
            {
                ESP_LOGW(TAG, "no pacing tick for 1 s");
                continue;
            }
            if (pending > 1)
            {
                self->overruns_ += pending - 1;
                if (self->overruns_ >= self->overrun_logged_ + 1000)
                {
                    self->overrun_logged_ = self->overruns_;
                    ESP_LOGW(TAG, "scan overrun: %u frames missed", (unsigned)self->overruns_);
                }
            }

            if (self->scan_.read_frame(frame, self->num_channels_) == ESP_OK)
            {
                // The auto-profiler is fed from here and nowhere else, because
                // this task owns the chain: a re-arm cannot read frames of its
                // own without two readers on one SPI device. Observation is
                // passive — counting highs and edges — so an armed profiler
                // costs the frame nothing it would not already spend.
                uint32_t word = self->profile_word_.load(std::memory_order_acquire);
                if (profile_phase(word) == ProfilerState::OBSERVING)
                {
                    // Arm on a pass number this task has not seen. Asking that
                    // question rather than keeping a flag is what makes an
                    // abandoned pass safe: a request always carries a new
                    // number, so the accumulators are always reset for it.
                    const uint32_t pass = profile_pass(word);
                    if (self->profile_pass_seen_ != pass)
                    {
                        self->profiler_.arm(self->profile_window_frames_);
                        self->profile_pass_seen_ = pass;
                    }
                    self->profiler_.observe(frame, self->num_channels_);
                    if (self->profiler_.window_complete() &&
                        // Compare-exchange, because the main task may have
                        // abandoned this pass. Losing means it did, and the
                        // result belongs to nobody.
                        self->profile_word_.compare_exchange_strong(
                            word, profile_pack(ProfilerState::CLASSIFYING, pass),
                            std::memory_order_acq_rel, std::memory_order_relaxed))
                    {
                        // The profiler's accumulators stop moving here —
                        // observe() ignores anything past the window — so the
                        // main task can read them without a lock and without
                        // this task being stopped.
                        xTaskNotifyGive(self->main_task_);
                    }
                }

                // Accumulated in plain locals — no atomics in the inner loop,
                // which is the whole reason the live hand-off is a bitmap
                // rather than a flag per channel (pinled_live.h).
                uint32_t active[LiveState::kWords]{};

                for (size_t ch = 0; ch < self->num_channels_; ++ch)
                {
                    self->filament_.update(ch, frame[ch]);
                    if (frame[ch])
                        active[ch / 32] |= (1u << (ch % 32));
                }
                self->filament_.snapshot(self->levels_, self->num_channels_);

                // Four atomics a frame regardless of how many lamps are lit.
                // Sticky, so the push task may be arbitrarily late without
                // losing anything (FR-UI-9).
                self->live_.publish(active);

#ifdef CONFIG_PINLED_SCAN_DEBUG
                for (size_t ch = 0; ch < self->num_channels_; ++ch)
                {
                    self->raw_snapshot_[ch] = frame[ch];
                    (frame[ch] ? self->seen_high_ : self->seen_low_)[ch] = 1;
                }
#endif
            }
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
            10000000, 13000000, 16000000, 20000000, 26000000, 33000000, 40000000};
        constexpr int kFramesPerRate = 256;
        constexpr int kDiscard = 8; // let the new divider settle

        bool frame[LampScan::MAX_CHANNELS];
        char bits[LampScan::MAX_CHANNELS + 2];

        // Only the first 64 channels are folded into the mask below, which is
        // ample for any bench rig and would silently ignore a longer chain.
        const size_t watched = num_channels_ < 64 ? num_channels_ : 64;

        for (;;)
        {
            ESP_LOGW(TAG, "=== chain clock sweep: %u channels (watching %u) ===",
                     (unsigned)num_channels_, (unsigned)watched);
            ESP_LOGW(TAG, "HOLD AN INPUT DOWN for the whole pass. With nothing pressed the "
                          "chain reads all-zeros at every rate, scores a perfect stability "
                          "result, and proves nothing whatever.");

            // The pattern the slowest working rate reads. Every faster rate is
            // judged against it, which is the half the first version of this
            // did not do: comparing a rate only against itself calls a rate
            // that reads the SAME WRONG BITS every frame perfectly stable.
            uint64_t reference = 0;
            bool have_reference = false;
            int best_hz = 0;      ///< fastest rate that matched the reference
            int best_actual = 0;
            int inconclusive = 0;
            /// A rate that failed *below* the fastest passing one. Then the
            /// fastest pass is a coincidence, not a ceiling, and quoting it as
            /// one would be how a marginal bus gets called good.
            bool failed_below_best = false;

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
                    for (size_t ch = 0; ch < watched; ++ch)
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
                if (reads == 0)
                {
                    ESP_LOGE(TAG, "%9d Hz: no frames read at all", kRates[r]);
                    continue;
                }

                size_t b = 0;
                for (size_t ch = 0; ch < watched; ++ch)
                    bits[b++] = (first >> ch) & 1 ? '1' : '0';
                bits[b] = '\0';

                const bool steady = (stable == reads) && (any == all);
                const int actual = scan_.actual_hz();

                // Zero examined is not a pass. A chain reading all-zeros is
                // perfectly stable and carries no evidence, so it is called
                // out as such rather than counted as clean.
                if (any == 0)
                {
                    ++inconclusive;
                    ESP_LOGW(TAG, "%9d Hz (actual %8d): %3d/%3d stable but ALL ZEROS "
                                  "— nothing held, proves nothing",
                             kRates[r], actual, stable, reads);
                    continue;
                }

                if (!steady)
                {
                    failed_below_best = have_reference;
                    ESP_LOGE(TAG, "%9d Hz (actual %8d): %3d/%3d stable  [%s]  UNSTABLE "
                                  "flapping=0x%llx",
                             kRates[r], actual, stable, reads, bits,
                             (unsigned long long)(any ^ all));
                    continue;
                }

                if (!have_reference)
                {
                    reference = first;
                    have_reference = true;
                    best_hz = kRates[r];
                    best_actual = actual;
                    ESP_LOGI(TAG, "%9d Hz (actual %8d): %3d/%3d stable  [%s]  "
                                  "REFERENCE (0x%llx)",
                             kRates[r], actual, stable, reads, bits,
                             (unsigned long long)reference);
                    continue;
                }

                if (first != reference)
                {
                    failed_below_best = true;
                    // Stable and wrong: the signature of a rate the chain
                    // cannot follow, rather than one it follows noisily. A
                    // dropped or doubled bit shifts the whole frame, so this
                    // usually reads as every channel moved by one.
                    ESP_LOGE(TAG, "%9d Hz (actual %8d): %3d/%3d stable  [%s]  "
                                  "MISMATCH — read 0x%llx, reference 0x%llx",
                             kRates[r], actual, stable, reads, bits,
                             (unsigned long long)first, (unsigned long long)reference);
                    continue;
                }

                best_hz = kRates[r];
                best_actual = actual;
                ESP_LOGI(TAG, "%9d Hz (actual %8d): %3d/%3d stable  [%s]  matches reference",
                         kRates[r], actual, stable, reads, bits);
            }

            if (!have_reference)
                ESP_LOGE(TAG, "=== sweep INCONCLUSIVE: %d rates read all-zeros. Hold an "
                              "input down and reset. ===",
                         inconclusive);
            else if (failed_below_best)
                ESP_LOGE(TAG, "=== sweep: %d Hz (actual %d) matched, but a SLOWER rate "
                              "failed — that is a marginal bus, not a ceiling. Trust the "
                              "fastest rate below the first failure. ===",
                         best_hz, best_actual);
            else
                ESP_LOGW(TAG, "=== sweep: clean to %d Hz (actual %d), every rate below it "
                              "matching ===",
                         best_hz, best_actual);

            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
#endif

#if CONFIG_PINLED_SCAN_HOLD_CH >= 0 || CONFIG_PINLED_SCAN_STEP_MS > 0
    // Where a global channel index physically lands. A '165 presents input H
    // first and shifts H,G,F,E,D,C,B,A, so channel 0 is wired to H -- and U1
    // (channels 0..7) sits at the head of each module's pair. Pin numbers are
    // from docs/BRINGUP.md §5 and are still unverified against a datasheet.
    struct ChannelSite
    {
        unsigned module;
        char chip;     ///< '1' for U1 (ch 0..7), '2' for U2 (ch 8..15)
        char input;    ///< A..H
        unsigned pin;  ///< DIP/SOIC pin number of that input
        unsigned local; ///< channel index within the module
    };

    static ChannelSite channel_site(unsigned ch, size_t channels_per_module)
    {
        static const char kOrder[] = "HGFEDCBA";
        static const unsigned kPins[] = {6, 5, 4, 3, 14, 13, 12, 11};
        const unsigned cpm = (unsigned)channels_per_module;
        const unsigned local = ch % cpm;
        return {ch / cpm, local < 8 ? '1' : '2', kOrder[local % 8],
                kPins[local % 8], local};
    }
#endif

#if CONFIG_PINLED_SCAN_HOLD_CH >= 0
    void Main::hold_channel()
    {
        const unsigned ch = CONFIG_PINLED_SCAN_HOLD_CH;
        if (ch >= num_channels_)
        {
            ESP_LOGE(TAG, "hold channel %u is past the configured %u channels",
                     ch, (unsigned)num_channels_);
            for (;;)
                vTaskDelay(pdMS_TO_TICKS(1000));
        }

        const ChannelSite s = channel_site(ch, cfg_.channels_per_module);

        if (ch == 0)
        {
            // FR-DIAG-3's useful special case: hold /PL low and the registers
            // stay transparent, so DATA tracks channel 0 continuously and a
            // meter follows the button in real time. Freezing a snapshot of
            // channel 0 would work too, and would tell you strictly less.
            scan_.hold_transparent();
            ESP_LOGI(TAG, "/PL held LOW: registers transparent, DATA tracks module 0 "
                          "ch 0 (U1 input H, pin %u) LIVE -- press the button and watch",
                     s.pin);
        }
        else
        {
            scan_.reload_chain();
            for (unsigned i = 0; i < ch; ++i)
                scan_.step_clock();
            ESP_LOGI(TAG, "chain parked on channel %u (module %u, U%c input %c, pin %u); "
                          "%u clocks issued",
                     ch, s.module, s.chip, s.input, s.pin, ch);
            ESP_LOGI(TAG, "DATA is now a static level -- trace input pin %u -> U1 QH "
                          "(pin 9) -> after R2 -> the MCU DATA pin; the first node that "
                          "stops tracking is the fault",
                     s.pin);
        }

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
        ESP_LOGI(TAG, "slow-step scan: %d ms/bit, %u channels",
                 CONFIG_PINLED_SCAN_STEP_MS, (unsigned)num_channels_);
        ESP_LOGI(TAG, "DATA is the only observable -- rev D has no counter to scope, "
                      "so the log names the pin each bit came from");

        for (;;)
        {
            ESP_LOGI(TAG, "--- pass start: /PL re-asserted before every step ---");

            for (size_t ch = 0; ch < num_channels_; ++ch)
            {
                vTaskDelay(pdMS_TO_TICKS(CONFIG_PINLED_SCAN_STEP_MS));

                // FR-DIAG-2: take a fresh snapshot and re-advance to bit ch
                // rather than walking one frozen frame, so an input toggled
                // part-way through the pass is still visible. The chain has no
                // addressing, so re-advancing means clocking from the head
                // every time -- free at 250 ms per step.
                scan_.reload_chain();
                for (size_t i = 0; i < ch; ++i)
                    scan_.step_clock();
                esp_rom_delay_us(1);

                const ChannelSite s = channel_site((unsigned)ch, cfg_.channels_per_module);
                ESP_LOGI(TAG, "bit %3u  -> module %u ch %2u  ('165 U%c pin %c)  DATA=%d",
                         (unsigned)ch, s.module, s.local, s.chip, s.input,
                         scan_.sample_now(0) ? 1 : 0);
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
        // Tasks do the work; this one classifies whatever the scan task has
        // finished observing.
        //
        // A notification rather than a poll, so a re-arm lands as soon as its
        // window fills instead of up to the timeout later — the person who
        // pressed the button is standing over the machine watching the lamps.
        // Measured: classification applied 1 ms after the window closes. The
        // timeout is still here because it costs nothing and a missed
        // notification would otherwise strand a completed pass forever.
        //
        // SCAN_DEBUG changes only the timeout and what gets logged, never the
        // hand-off. It used to have a loop of its own that slept instead of
        // waiting, which made the bring-up build the one place a re-arm could
        // take half a second to land — and since the bench runs that build, it
        // was the only path ever measured. Two behaviours behind one Kconfig
        // symbol is how half of a fork goes unverified.
#ifdef CONFIG_PINLED_SCAN_DEBUG
        constexpr TickType_t kIdlePeriod = pdMS_TO_TICKS(500); // the raw/tog cadence
#else
        constexpr TickType_t kIdlePeriod = pdMS_TO_TICKS(5000); // just a heartbeat
#endif
        for (;;)
        {
            ulTaskNotifyTake(pdTRUE, kIdlePeriod);
            service_profile();
#ifdef CONFIG_PINLED_SCAN_DEBUG
            // The raw bus, so bring-up has something to look at that does not
            // depend on the LED path working.
            log_scan_debug();
#else
            ESP_LOGD(TAG, "tick");
#endif
        }
    }
} // namespace ooe::pinled
