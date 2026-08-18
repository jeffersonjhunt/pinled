/**
 * @file indicator.cpp
 * @brief The status pixel: one RGB LED, driven from its own task (FR-IND-1..8).
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "indicator.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "neopixel.h"

namespace ooe::pinled
{
    static const char *TAG = "indicator";

    namespace
    {
        /// 100 Hz. Fast enough that the 80 ms press blip is never missed --
        /// FR-IND-7's whole value is that the acknowledgement is immediate,
        /// and a 50 ms tick could swallow it -- and slow enough to be
        /// invisible on a core that also runs the renderer.
        constexpr TickType_t kTick = pdMS_TO_TICKS(10);
    } // namespace

    uint32_t Indicator::now_ms()
    {
        return static_cast<uint32_t>(esp_timer_get_time() / 1000);
    }

    esp_err_t Indicator::init(const IndicatorConfig &cfg)
    {
        if (cfg.pin == GPIO_NUM_NC)
            return ESP_ERR_INVALID_ARG;

        deinit();
        cfg_ = cfg;
        brightness_.store(cfg.brightness, std::memory_order_relaxed);
        state_at_.store(now_ms(), std::memory_order_relaxed);

        if (cfg_.power_pin != GPIO_NUM_NC)
        {
            gpio_config_t io{};
            io.pin_bit_mask = 1ULL << static_cast<unsigned>(cfg_.power_pin);
            io.mode = GPIO_MODE_OUTPUT;
            const esp_err_t err = gpio_config(&io);
            if (err != ESP_OK)
                return err;
            gpio_set_level(cfg_.power_pin, 1);
            // The rail has to come up before the first bit is clocked out,
            // or the pixel latches whatever it saw while browning out.
            vTaskDelay(pdMS_TO_TICKS(2));
        }

        tNeopixelContext ctx = neopixel_Init(1, cfg_.pin);
        if (ctx == nullptr)
        {
            ESP_LOGE(TAG, "neopixel_Init failed (pin %d)", (int)cfg_.pin);
            return ESP_FAIL;
        }
        neopixel_ = ctx;

        running_.store(true, std::memory_order_relaxed);

        // Lowest useful priority, and on core 0 -- the core the scan does not
        // own (FR-IND-6). The stack is small because the task allocates
        // nothing: it reads atomics, calls a pure function, and writes one
        // pixel.
        if (xTaskCreatePinnedToCore(task, "pinled_status", 2560, this, 2, nullptr, 0) != pdPASS)
        {
            running_.store(false, std::memory_order_relaxed);
            neopixel_Deinit(ctx);
            neopixel_ = nullptr;
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "status pixel on GPIO %d%s, brightness %u/255, byte order %s",
                 (int)cfg_.pin,
                 cfg_.power_pin == GPIO_NUM_NC ? "" : " (power enabled)",
                 (unsigned)cfg_.brightness, color_order_str(cfg_.color_order));
        return ESP_OK;
    }

    void Indicator::deinit()
    {
        running_.store(false, std::memory_order_relaxed);
        if (neopixel_)
        {
            // The task notices `running_` within one tick and returns; give it
            // that long before the context it is using goes away.
            vTaskDelay(kTick * 2);
            neopixel_Deinit(static_cast<tNeopixelContext>(neopixel_));
            neopixel_ = nullptr;
        }
    }

    void Indicator::set_state(IndicatorState s)
    {
        const uint8_t want = static_cast<uint8_t>(s);
        // Re-entering the same state must not restart its pattern: a caller
        // re-asserting RUNNING on a timer would otherwise hold the offline
        // breathe at the start of its rise forever.
        uint8_t was = state_.exchange(want, std::memory_order_relaxed);
        if (was == want)
            return;
        state_at_.store(now_ms(), std::memory_order_relaxed);
        ESP_LOGI(TAG, "%s", indicator_state_name(s));
    }

    void Indicator::set_fault(FaultClass c, bool active)
    {
        const uint8_t bit = fault_bit(c);
        if (bit == 0)
            return;
        const uint8_t before =
            active ? faults_.fetch_or(bit, std::memory_order_relaxed)
                   : faults_.fetch_and(static_cast<uint8_t>(~bit), std::memory_order_relaxed);
        const uint8_t after = active ? static_cast<uint8_t>(before | bit)
                                     : static_cast<uint8_t>(before & ~bit);
        if (before == after)
            return;

        // Restart the pattern whenever the count changes, so the blinks a
        // person is watching are a whole cycle of the class now being shown
        // rather than the tail of the previous one (FR-IND-4).
        if (lowest_fault(before) != lowest_fault(after))
            state_at_.store(now_ms(), std::memory_order_relaxed);

        ESP_LOGW(TAG, "fault %u (%s) %s; showing %u",
                 (unsigned)c, fault_class_name(c), active ? "raised" : "cleared",
                 (unsigned)lowest_fault(after));
    }

    void Indicator::note_press() { blip_at_.store(now_ms(), std::memory_order_relaxed); }
    void Indicator::note_profiling() { flash_at_.store(now_ms(), std::memory_order_relaxed); }

    void Indicator::set_staged(uint32_t window_ms)
    {
        staged_window_.store(window_ms, std::memory_order_relaxed);
        staged_until_.store(now_ms() + window_ms, std::memory_order_relaxed);
        set_state(IndicatorState::STAGED);
    }

    void Indicator::clear_staged()
    {
        staged_until_.store(0, std::memory_order_relaxed);
        staged_window_.store(0, std::memory_order_relaxed);
    }

    IndicatorInput Indicator::snapshot() const
    {
        const uint32_t now = now_ms();

        IndicatorInput in{};
        in.state = static_cast<IndicatorState>(state_.load(std::memory_order_relaxed));
        in.state_ms = now - state_at_.load(std::memory_order_relaxed);
        in.faults = faults_.load(std::memory_order_relaxed);
        in.brightness = brightness_.load(std::memory_order_relaxed);
        in.hold_ms = hold_ms_.load(std::memory_order_relaxed);
        in.hold_erase_ms = cfg_.hold_erase_ms;

        in.blip_ms = now - blip_at_.load(std::memory_order_relaxed);
        in.flash_ms = now - flash_at_.load(std::memory_order_relaxed);

        const uint32_t until = staged_until_.load(std::memory_order_relaxed);
        in.staged_window_ms = staged_window_.load(std::memory_order_relaxed);
        // Signed difference so a deadline that has already passed reads as
        // zero remaining rather than as 49 days of it.
        const int32_t left = static_cast<int32_t>(until - now);
        in.staged_remaining_ms = left > 0 ? static_cast<uint32_t>(left) : 0;
        return in;
    }

    void Indicator::task(void *arg) { static_cast<Indicator *>(arg)->run(); }

    void Indicator::run()
    {
        TickType_t last = xTaskGetTickCount();
        uint32_t shown = 0xFFFFFFFFu; // impossible packed value: force a first write

        while (running_.load(std::memory_order_relaxed))
        {
            const IndicatorRgb c = indicator_render(snapshot());
            const uint32_t rgb = pack_for_order(cfg_.color_order, c.r, c.g, c.b);

            // Only transmit on change. At 100 Hz an unconditional write would
            // be 100 RMT transactions a second to say nothing, and a healthy
            // machine is a steady colour for months at a time.
            if (rgb != shown)
            {
                tNeopixel px{0, rgb};
                if (neopixel_SetPixel(static_cast<tNeopixelContext>(neopixel_), &px, 1))
                    shown = rgb;
                // A failed write is simply retried next tick. FR-IND-6: a
                // dropped update is of no consequence, and there is nobody to
                // report it to who is not already looking at this pixel.
            }

            vTaskDelayUntil(&last, kTick);
        }
        vTaskDelete(nullptr);
    }
} // namespace ooe::pinled
