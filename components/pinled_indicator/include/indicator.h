#pragma once

/**
 * @file indicator.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief The status pixel: one RGB LED, driven from its own task (FR-IND-1..8).
 * @version 0.2.0
 * @date 2026-08-18
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * The thin half. Everything about *what* is shown lives in
 * `pinled_indicator.h`, which has no IDF dependency and is tested on the
 * host; this owns the pin, the task and the clock, and does nothing a test
 * would want to assert.
 *
 * @par Every setter is a store, and none of them blocks
 * FR-IND-6: the indicator is not on the scan or render path, and a dropped
 * update is of no consequence. These are called from the button task, the
 * httpd task and the main task, so a setter that could block would put the
 * indicator in front of things that matter. They write atomics and return;
 * the task picks the change up within one tick of its own cadence.
 *
 * @par Why the state's start time is a member and not a parameter
 * Every periodic pattern is keyed off "how long since this state was
 * entered", which is the property that makes a fault count from its first
 * blink rather than from wherever a free-running clock happened to be
 * (FR-IND-4). Entering the same state twice is therefore NOT a no-op in
 * general -- but it is treated as one here, because a caller re-asserting
 * RUNNING every few seconds must not restart a breathe halfway through.
 */

#include <atomic>
#include <cstdint>

#include "esp_err.h"

#include "driver/gpio.h"

#include "pinled_color_order.h"
#include "pinled_indicator.h"

namespace ooe::pinled
{
    struct IndicatorConfig
    {
        /// Data pin for the single pixel. Its own GPIO, never the first pixel
        /// of the playfield string (FR-IND-1, HW-15).
        gpio_num_t pin{GPIO_NUM_NC};

        /// Power-enable for the pixel, driven high at init. The QT Py wires
        /// its onboard NeoPixel this way (GPIO 38 for the pixel on 39) and it
        /// is dark without it, which looks exactly like a dead driver.
        /// GPIO_NUM_NC where the pixel is always powered.
        gpio_num_t power_pin{GPIO_NUM_NC};

        /// Byte order of this pixel, independent of the playfield strip's:
        /// they are different parts on different boards.
        ColorOrder color_order{ColorOrder::UNSPECIFIED};

        uint8_t brightness{128}; ///< FR-IND-5, 0 = off

        /// The erase threshold the hold ramp runs to, so the ramp reaches its
        /// fastest exactly as the credentials go (FR-IND-8).
        uint32_t hold_erase_ms{5000};
    };

    class Indicator
    {
    public:
        esp_err_t init(const IndicatorConfig &cfg);
        void deinit();

        /// True once the pixel is driven. Every setter is safe to call before
        /// this -- they are stores, and the state they build is simply shown
        /// from the moment the task starts.
        bool ready() const { return running_.load(std::memory_order_relaxed); }

        /// Enter a base state. Re-entering the current state does not restart
        /// its pattern; see the note above.
        void set_state(IndicatorState s);
        IndicatorState state() const
        {
            return static_cast<IndicatorState>(state_.load(std::memory_order_relaxed));
        }

        /// Raise or clear one fault class. A bitmap rather than a value
        /// because several can be true at once: the pixel shows the lowest,
        /// and `faults()` is what the API reports in full (FR-IND-4).
        void set_fault(FaultClass c, bool active);
        uint8_t faults() const { return faults_.load(std::memory_order_relaxed); }

        void set_brightness(uint8_t b) { brightness_.store(b, std::memory_order_relaxed); }

        /// FR-IND-7. Called the instant a press passes the debounce
        /// threshold, before and independently of whatever it goes on to do.
        void note_press();

        /// One brief white flash: a profiler pass has begun.
        void note_profiling();

        /// FR-IND-8. How long the button has been held, or 0 on release.
        void set_hold(uint32_t held_ms) { hold_ms_.store(held_ms, std::memory_order_relaxed); }

        /// An image is staged and the confirmation window is open (FR-OTA-2).
        /// The deadline is stored, not the remaining time, so the OTA path
        /// sets this once and the countdown runs itself.
        void set_staged(uint32_t window_ms);
        void clear_staged();

        /// What the pixel is showing right now, for logging and the API.
        IndicatorInput snapshot() const;

    private:
        static void task(void *arg);
        void run();

        /// Milliseconds since boot. Same base as every stored instant here,
        /// and unsigned throughout, so the 49-day wrap costs one frame rather
        /// than a stuck pattern.
        static uint32_t now_ms();

        IndicatorConfig cfg_{};

        // Opaque so the header does not drag driver/rmt_tx.h into everything
        // that includes it -- which is main.h, and from there the whole app.
        void *channel_{nullptr}; ///< rmt_channel_handle_t
        void *encoder_{nullptr}; ///< rmt_encoder_handle_t

        std::atomic<bool> running_{false};

        std::atomic<uint8_t> state_{static_cast<uint8_t>(IndicatorState::BOOTING)};
        std::atomic<uint32_t> state_at_{0};

        std::atomic<uint8_t> faults_{0};
        std::atomic<uint8_t> brightness_{128};

        // Absolute instants, not elapsed times: a setter must not have to know
        // what "now" is, and 0 -- the value these hold when nothing has ever
        // happened -- is already long past by the time the indicator starts,
        // so it reads as an expired overlay without needing a sentinel.
        std::atomic<uint32_t> blip_at_{0};
        std::atomic<uint32_t> flash_at_{0};

        std::atomic<uint32_t> hold_ms_{0};

        std::atomic<uint32_t> staged_until_{0};
        std::atomic<uint32_t> staged_window_{0};
    };
} // namespace ooe::pinled
