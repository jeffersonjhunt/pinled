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
