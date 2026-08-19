/**
 * @file rescue.cpp
 * @brief Button-held return to SoftAP (FR-UI-7).
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "rescue.h"

#include <new>

#include "credentials.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace ooe::pinled
{
    namespace
    {
        const char *TAG = "rescue";

        struct Args
        {
            gpio_num_t pin;
            uint32_t hold_ms;
            void (*on_short_press)(void *);
            void *arg;
            ButtonFeedback feedback;
        };

        /// 20 Hz. A person pressing a button thinks in tens of milliseconds,
        /// and at the original 10 Hz an ordinary click could land entirely
        /// between two samples — which it did on the bench, invisibly.
        constexpr uint32_t kPollMs = 50;
        constexpr TickType_t kPoll = pdMS_TO_TICKS(kPollMs);

        /// Below this a press is contact bounce or a knock rather than an
        /// intent. Three samples, and deliberately chosen to sit BELOW an
        /// ordinary click rather than above it: the first value was 300 ms,
        /// which is longer than most people press a button for, so the
        /// re-arm ignored every normal press and said nothing about it.
        constexpr uint32_t kShortPressMinMs = 150;

        void rescue_task(void *arg)
        {
            const Args cfg = *static_cast<Args *>(arg);
            delete static_cast<Args *>(arg);

            uint32_t held = 0;
            uint32_t announced = 0;

            for (;;)
            {
                vTaskDelay(kPoll);

                if (gpio_get_level(cfg.pin) != 0) // active low
                {
                    // A release before the threshold is a SHORT press: the
                    // profiler re-arm. Acting on RELEASE rather than at 300 ms
                    // is what keeps the two jobs on one button honest — at the
                    // moment the threshold passes there is no way to know
                    // whether this is a short press or the first third of a
                    // long hold, and re-profiling on the way to erasing the
                    // network would be a surprise every time.
                    if (held >= kShortPressMinMs && held < cfg.hold_ms)
                    {
                        if (cfg.on_short_press)
                            cfg.on_short_press(cfg.arg);
                        else
                            ESP_LOGI(TAG, "short press ignored; hold %u s to reset the network",
                                     (unsigned)(cfg.hold_ms / 1000));
                    }
                    else if (held > 0)
                    {
                        // Detected, and too brief to act on. Saying so is the
                        // whole point: a press under the threshold produced no
                        // countdown, no callback and no log line, so "pressed
                        // it too quickly" and "the button is dead" were the
                        // same observation. That cost a bench session.
                        ESP_LOGI(TAG, "press of %u ms was too short; hold it for %u ms",
                                 (unsigned)held, (unsigned)kShortPressMinMs);
                    }
                    // Only on an actual release: while the button sits idle
                    // this branch runs every poll, and the contract is one
                    // zero per release, not twenty a second forever.
                    if (held > 0 && cfg.feedback.held)
                        cfg.feedback.held(cfg.feedback.arg, 0);
                    held = 0;
                    announced = 0;
                    continue;
                }

                // Derived from the poll period rather than restated. These
                // were two independent constants that happened to agree, so
                // changing the poll rate silently rescaled every threshold
                // measured against `held`.
                const uint32_t before = held;
                held += kPollMs;

                // FR-IND-7, and the reason this is here rather than beside the
                // short-press callback below: the acknowledgement belongs to
                // the press, not to what the press turns out to mean. At this
                // instant that is still unknown — this could equally be the
                // first tenth of a hold that erases the network.
                if (before < kShortPressMinMs && held >= kShortPressMinMs &&
                    cfg.feedback.accepted)
                    cfg.feedback.accepted(cfg.feedback.arg);
                if (cfg.feedback.held)
                    cfg.feedback.held(cfg.feedback.arg, held);

                // Counted down out loud, because the only other feedback is a
                // reboot and there is no way to tell "holding it long enough"
                // from "the button does nothing" while you are doing it.
                if (held / 1000 != announced / 1000 && held < cfg.hold_ms)
                    ESP_LOGW(TAG, "hold %u more second(s) to erase the network",
                             (unsigned)((cfg.hold_ms - held + 999) / 1000));
                announced = held;

                if (held >= cfg.hold_ms)
                {
                    ESP_LOGW(TAG, "rescue: erasing credentials and restarting into SoftAP");
                    credentials_erase();
                    vTaskDelay(pdMS_TO_TICKS(200)); // let the log drain
                    esp_restart();
                }
            }
        }
    } // namespace

    esp_err_t rescue_button_start(int gpio, uint32_t hold_ms,
                                  void (*on_short_press)(void *), void *arg,
                                  const ButtonFeedback *feedback)
    {
        if (gpio < 0)
            return ESP_ERR_INVALID_ARG;

        gpio_config_t io{};
        io.pin_bit_mask = 1ULL << static_cast<unsigned>(gpio);
        io.mode = GPIO_MODE_INPUT;
        io.pull_up_en = GPIO_PULLUP_ENABLE;
        const esp_err_t err = gpio_config(&io);
        if (err != ESP_OK)
            return err;

        auto *args = new (std::nothrow) Args{static_cast<gpio_num_t>(gpio), hold_ms,
                                             on_short_press, arg,
                                             feedback ? *feedback : ButtonFeedback{}};
        if (args == nullptr)
            return ESP_ERR_NO_MEM;

        // Polling at 10 Hz on the lowest useful priority. An interrupt would be
        // tidier and would buy nothing: the event being detected is measured in
        // seconds.
        if (xTaskCreatePinnedToCore(rescue_task, "pinled_rescue", 3072, args,
                                    2, nullptr, 0) != pdPASS)
        {
            delete args;
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "button on GPIO %d: short press %s, hold %u s clears the network",
                 gpio, on_short_press ? "re-profiles" : "ignored",
                 (unsigned)(hold_ms / 1000));
        return ESP_OK;
    }
} // namespace ooe::pinled
