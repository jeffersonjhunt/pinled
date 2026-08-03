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

#include "lamp_scan.h"
#include "filament.h"
#include "profiler.h"
#include "lamp_map.h"
#include "machine_config.h"

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

        static void scan_task(void *arg);
        static void render_task(void *arg);

#ifdef CONFIG_PINLED_SPI_SWEEP
        /// Bring-up only: step the chain clock and report where reads stop
        /// being stable. Owns the scan hardware; never returns.
        void spi_sweep();
#endif

#if CONFIG_PINLED_SCAN_HOLD_CH >= 0
        /// Bring-up only: park the counter on one channel so every node in the
        /// chain is a static level a meter can read. Never returns.
        void hold_channel();
#endif

#if CONFIG_PINLED_SCAN_STEP_MS > 0
        /// Bring-up only: walk the counter one channel at a time, slowly enough
        /// to watch. Owns the counter, so scan_task is not started. Never returns.
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

        MachineConfig cfg_{};
        MachineConfigStore store_{};
        LampScan scan_{};
        Filament filament_{};
        Profiler profiler_{};
        LampMap map_{};

        // Shared brightness buffer: scan_task writes, render_task reads.
        uint8_t levels_[LampScan::MAX_CHANNELS]{};
        size_t num_channels_{0};

        TaskHandle_t scan_task_{nullptr};
        TaskHandle_t render_task_{nullptr};
    };
} // namespace ooe::pinled
