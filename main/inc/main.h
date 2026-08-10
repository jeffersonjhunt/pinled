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

#include "pinled_apply.h"
#include "pinled_channel_config.h"

#include "version.h"

namespace ooe::pinled
{
    /// Top-level application. Owns the config, scan driver, filament bank,
    /// profiler, and LED map, and wires up the scan/render tasks.
    class Main
    {
    public:
        esp_err_t init();
        void run();

    private:
        void version();
        esp_err_t start_tasks();
        void profile_boot(); ///< run the boot-time auto-profiler pass

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
        LampScan scan_{};
        Filament filament_{};
        Profiler profiler_{};
        LampMap map_{};

        /// Resolved per-channel configuration. Built from Kconfig defaults
        /// today; step 4 loads it from storage instead. 2 KB at 128 channels,
        /// which is why it is a member and not a stack array — the boot path
        /// already overflows the main task stack with less than this.
        ChannelConfig channels_[LampScan::MAX_CHANNELS]{};

        // Shared brightness buffer: scan_task writes, render_task reads.
        uint8_t levels_[LampScan::MAX_CHANNELS]{};
        size_t num_channels_{0};

        TaskHandle_t scan_task_{nullptr};
        TaskHandle_t render_task_{nullptr};

        gptimer_handle_t sample_timer_{nullptr};
        float fs_actual_{0.0f};      ///< Fs after the boot clamp; the real one
        uint32_t overruns_{0};       ///< frames the scan task failed to keep up with
        uint32_t overrun_logged_{0}; ///< rate-limits the overrun warning
    };
} // namespace ooe::pinled
