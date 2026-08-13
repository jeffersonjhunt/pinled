#pragma once

/**
 * @file main.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief pinled v2 application: sense pinball lamp drive, reconstruct
 *        brightness through a filament model, drive addressable LEDs.
 * @version 0.2.0
 * @date 2026-07-16
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * See docs/DOSSIER.md and docs/FIRMWARE_PLAN.md for the full design.
 */

#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_log.h"

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gptimer.h"

#include "lamp_scan.h"
#include "filament.h"
#include "profiler.h"
#include "lamp_map.h"
#include "machine_config.h"

#include "api.h"
#include "net.h"
#include "rescue.h"

#include "pinled_apply.h"
#include "pinled_channel_config.h"
#include "pinled_live.h"
#include "pinled_profiling.h"

#include "version.h"

#include <atomic>

namespace ooe::pinled
{
    /// Top-level application. Owns the config, scan driver, filament bank,
    /// profiler, and LED map, and wires up the scan/render tasks.
    ///
    /// Implements ProfilerControl so the API can ask for a fresh
    /// classification without knowing anything else about the application.
    class Main : public ProfilerControl
    {
    public:
        esp_err_t init();
        void run();

        // --- ProfilerControl -------------------------------------------------
        //
        // Called from the httpd task and from the button task. Both do nothing
        // but post a request and read atomics; the work happens on the scan
        // task and the main task.

        bool rearm_profiler() override;
        ProfilerStatus profiler_status() const override;

    private:
        void version();
        esp_err_t start_tasks();

        /**
         * @brief Request a pass and wait for it to land.
         *
         * Boot only. Everywhere else a re-arm is asynchronous — the caller is
         * a web request or a button press and has nothing useful to do while
         * the window fills. Boot does: it wants the machine classified before
         * the API can be asked about it.
         */
        void profile_and_wait(const char *reason);

        /**
         * @brief Classify a completed window and push the result into the
         *        filament bank and the live monitor.
         *
         * Runs on the main task, never the scan task: it allocates, it does
         * floating point, and it must not sit in front of a 10 kHz frame.
         * Safe without a lock only because the scan task stops feeding the
         * profiler the moment its window is full — see Profiler::observe().
         */
        void apply_classification();

        /// Poll the hand-off and finish any completed window. Called from the
        /// main loop on every wake-up, notification or timeout alike.
        void service_profile();

        /// Bring up Wi-Fi and the HTTP API. Last, and deliberately not fatal:
        /// the lamps are the product and they must work on a bench with no
        /// network at all.
        void start_network();

        /// The rescue / re-arm button. Not a network feature, despite where it
        /// is started from: a short press re-profiles (FR-PROF-2), a long hold
        /// erases the stored network (FR-UI-7).
        void start_button();

        /// FR-SCAN-9: measure what the scan hardware actually sustains and
        /// clamp the configured sample rate to it. The clamped value becomes
        /// the authoritative Fs for every downstream time constant.
        esp_err_t measure_and_clamp_fs();

        /// FR-LED-8: clamp `refresh_hz` to what the strip length allows.
        void clamp_refresh();

        /// FR-SCAN-8: start the gptimer that paces one frame per tick.
        esp_err_t start_pacing();

        static void scan_task(void *arg);
        static void render_task(void *arg);

#ifdef CONFIG_PINLED_SPI_SWEEP
        /// Bring-up only: step the chain clock and report where reads stop
        /// being stable. Owns the scan hardware; never returns.
        void spi_sweep();
#endif

#if CONFIG_PINLED_SCAN_HOLD_CH >= 0
        /// Bring-up only: snapshot the chain and clock it to one channel, so
        /// every node on the serial path is a static level a meter can read.
        /// Channel 0 instead holds `/PL` low and tracks live (FR-DIAG-3).
        /// Owns the chain, so scan_task is not started. Never returns.
        void hold_channel();
#endif

#if CONFIG_PINLED_SCAN_STEP_MS > 0
        /// Bring-up only: walk the serial stream one bit at a time, slowly
        /// enough to watch, re-snapshotting before each step. Owns the chain,
        /// so scan_task is not started. Never returns.
        void step_walk();
#endif

#ifdef CONFIG_PINLED_SCAN_DEBUG
        /// Bring-up aid: dump the raw pre-filament frame. Removed by M1a, which
        /// replaces scan_task wholesale.
        void log_scan_debug();

        uint8_t raw_snapshot_[LampScan::MAX_CHANNELS]{}; ///< last raw frame
        uint8_t seen_high_[LampScan::MAX_CHANNELS]{};    ///< channel ever read 1
        uint8_t seen_low_[LampScan::MAX_CHANNELS]{};     ///< channel ever read 0
#endif

        /// Push `channels_` into the filament bank and the LED map. The single
        /// place a configuration becomes running behaviour (FR-CFG-8).
        void apply_channel_config();

        MachineConfig cfg_{};
        MachineConfigStore store_{};
        Net net_{};
        Api api_{};
        LampScan scan_{};
        Filament filament_{};
        Profiler profiler_{};
        LampMap map_{};

        /// Resolved per-channel configuration, filled by the store from the
        /// two documents or from Kconfig defaults. 2 KB at 128 channels, which
        /// is why it is a member and not a stack array — the boot path already
        /// overflows the main task stack with less than this.
        ChannelConfig channels_[LampScan::MAX_CHANNELS]{};

        /// Last classification per channel, kept for the live monitor.
        DriveClass classes_[LampScan::MAX_CHANNELS]{};

        /// Scan -> push hand-off (FR-UI-5/9). Borrows levels_, channels_ and
        /// classes_; owns only the sticky bitmap.
        LiveState live_{};

        // Shared brightness buffer: scan_task writes, render_task reads.
        uint8_t levels_[LampScan::MAX_CHANNELS]{};
        size_t num_channels_{0};

        TaskHandle_t scan_task_{nullptr};
        TaskHandle_t render_task_{nullptr};
        /// Notified by the scan task when an observation window completes.
        TaskHandle_t main_task_{nullptr};

        // --- auto-profiler hand-off (FR-PROF-2) ------------------------------
        //
        // Three tasks touch this and none of them takes a lock, which works
        // because each owns a distinct phase and the state is the baton:
        //
        //   requester (httpd / button)  IDLE -> OBSERVING   (compare-exchange)
        //   scan task                   arms, feeds, then
        //                               OBSERVING -> CLASSIFYING + notify
        //   main task                   classifies, applies,
        //                               CLASSIFYING -> IDLE
        //
        // The compare-exchange is what makes a second request while a pass is
        // running a refusal rather than a restart of the window — two people
        // pressing the button and clicking the button should not silently
        // extend how long either waits.
        std::atomic<ProfilerState> profile_state_{ProfilerState::IDLE};
        std::atomic<uint32_t> profile_passes_{0};

        /// Scan task only: whether it has armed the profiler for the pass it
        /// is currently servicing. A plain bool because exactly one task ever
        /// reads or writes it.
        bool profile_armed_{false};

        /// Window length in frames, derived from the MEASURED Fs (FR-PROF-5).
        uint32_t profile_window_frames_{0};

        gptimer_handle_t sample_timer_{nullptr};

        float fs_actual_{0.0f};      ///< Fs after the boot clamp; the real one
        uint32_t overruns_{0};       ///< frames the scan task failed to keep up with
        uint32_t overrun_logged_{0}; ///< rate-limits the overrun warning
    };
} // namespace ooe::pinled
