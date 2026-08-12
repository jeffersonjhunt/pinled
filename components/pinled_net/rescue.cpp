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
        };

        constexpr TickType_t kPoll = pdMS_TO_TICKS(100);

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
                    // A release before the threshold is a SHORT press, which
                    // this deliberately does nothing with — it belongs to the
                    // profiler re-arm. Saying so out loud beats leaving
                    // someone to wonder whether the button works.
                    if (held >= 300 && held < cfg.hold_ms)
                        ESP_LOGI(TAG, "short press ignored; hold %u s to reset the network",
                                 (unsigned)(cfg.hold_ms / 1000));
                    held = 0;
                    announced = 0;
                    continue;
                }

                held += 100;

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

    esp_err_t rescue_button_start(int gpio, uint32_t hold_ms)
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

        auto *args = new (std::nothrow) Args{static_cast<gpio_num_t>(gpio), hold_ms};
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
        ESP_LOGI(TAG, "rescue: hold GPIO %d for %u s to clear the network",
                 gpio, (unsigned)(hold_ms / 1000));
        return ESP_OK;
    }
} // namespace ooe::pinled
