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

#include "driver/rmt_tx.h"

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

        // WS2812B over RMT, and NOT through the zorxx/neopixel component the
        // playfield string uses -- which is an I2S driver, not an RMT one.
        //
        // The ESP32-S3 has two I2S controllers and four RMT TX channels. The
        // first version of this file took the second I2S controller for one
        // pixel updated a few times a second, which cost the scarcer
        // peripheral to save thirty lines and left nothing for anything else
        // that might want I2S. The board said so on the first boot:
        //
        //   W i2s_platform: i2s controller 0 has been occupied by i2s_driver
        //
        // It worked. It was still the wrong half of the budget to spend.
        //
        // 10 MHz resolution, so one tick is 100 ns and the WS2812B bit
        // timings fall on whole ticks with margin either side.
        constexpr uint32_t kRmtHz = 10 * 1000 * 1000;
        constexpr uint16_t kT0H = 3; ///< 0.3 us
        constexpr uint16_t kT0L = 9; ///< 0.9 us
        constexpr uint16_t kT1H = 9; ///< 0.9 us
        constexpr uint16_t kT1L = 3; ///< 0.3 us
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

        rmt_tx_channel_config_t tx{};
        tx.gpio_num = cfg_.pin;
        tx.clk_src = RMT_CLK_SRC_DEFAULT;
        tx.resolution_hz = kRmtHz;
        // The smallest block the hardware allocates. One pixel is 24 bits and
        // the encoder streams, so nothing here needs more.
        tx.mem_block_symbols = 64;
        tx.trans_queue_depth = 2;

        rmt_channel_handle_t chan = nullptr;
        esp_err_t err = rmt_new_tx_channel(&tx, &chan);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "rmt_new_tx_channel on GPIO %d: %s",
                     (int)cfg_.pin, esp_err_to_name(err));
            return err;
        }

        rmt_bytes_encoder_config_t enc{};
        enc.bit0.level0 = 1;
        enc.bit0.duration0 = kT0H;
        enc.bit0.level1 = 0;
        enc.bit0.duration1 = kT0L;
        enc.bit1.level0 = 1;
        enc.bit1.duration0 = kT1H;
        enc.bit1.level1 = 0;
        enc.bit1.duration1 = kT1L;
        enc.flags.msb_first = 1; // WS2812B takes the high bit of each byte first

        rmt_encoder_handle_t encoder = nullptr;
        err = rmt_new_bytes_encoder(&enc, &encoder);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "rmt_new_bytes_encoder: %s", esp_err_to_name(err));
            rmt_del_channel(chan);
            return err;
        }

        err = rmt_enable(chan);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "rmt_enable: %s", esp_err_to_name(err));
            rmt_del_encoder(encoder);
            rmt_del_channel(chan);
            return err;
        }

        channel_ = chan;
        encoder_ = encoder;

        running_.store(true, std::memory_order_relaxed);
        exited_.store(false, std::memory_order_relaxed);

        // Lowest useful priority, and on core 0 -- the core the scan does not
        // own (FR-IND-6). The stack is small because the task allocates
        // nothing: it reads atomics, calls a pure function, and writes one
        // pixel.
        if (xTaskCreatePinnedToCore(task, "pinled_status", 2560, this, 2, nullptr, 0) != pdPASS)
        {
            running_.store(false, std::memory_order_relaxed);
            exited_.store(true, std::memory_order_relaxed);
            rmt_disable(chan);
            rmt_del_encoder(encoder);
            rmt_del_channel(chan);
            channel_ = nullptr;
            encoder_ = nullptr;
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "status pixel on GPIO %d%s (RMT), brightness %u/255, byte order %s",
                 (int)cfg_.pin,
                 cfg_.power_pin == GPIO_NUM_NC ? "" : " (power enabled)",
                 (unsigned)cfg_.brightness, color_order_str(cfg_.color_order));
        return ESP_OK;
    }

    void Indicator::deinit()
    {
        running_.store(false, std::memory_order_relaxed);
        if (channel_)
        {
            // Free nothing until the task says it is out: it may be inside a
            // transmit whose completion wait is bounded at 100 ms, so a fixed
            // grace shorter than that is a use-after-free with good timing.
            // The 500 ms ceiling only matters if the task died without
            // reporting, at which point leaking beats freeing under it.
            for (int i = 0; i < 50 && !exited_.load(std::memory_order_acquire); ++i)
                vTaskDelay(kTick);
            rmt_disable(static_cast<rmt_channel_handle_t>(channel_));
            rmt_del_encoder(static_cast<rmt_encoder_handle_t>(encoder_));
            rmt_del_channel(static_cast<rmt_channel_handle_t>(channel_));
            channel_ = nullptr;
            encoder_ = nullptr;
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

        rmt_transmit_config_t tx{};
        tx.loop_count = 0;

        while (running_.load(std::memory_order_relaxed))
        {
            const IndicatorRgb c = indicator_render(snapshot());

            // Only transmit on change. At 100 Hz an unconditional write would
            // be 100 transactions a second to say nothing, and a healthy
            // machine is a steady colour for months at a time. Comparing the
            // packed word rather than the struct keeps that check to one
            // integer compare.
            const uint32_t rgb = pack_for_order(cfg_.color_order, c.r, c.g, c.b);
            if (rgb != shown)
            {
                uint8_t wire[3]{};
                bytes_for_order(cfg_.color_order, c.r, c.g, c.b, wire);

                const esp_err_t err =
                    rmt_transmit(static_cast<rmt_channel_handle_t>(channel_),
                                 static_cast<rmt_encoder_handle_t>(encoder_),
                                 wire, sizeof(wire), &tx);
                if (err == ESP_OK)
                {
                    // Wait, so the next tick cannot queue behind an in-flight
                    // frame. 24 bits at 1.25 us is 30 us; the 100 ms bound is
                    // there so a wedged peripheral cannot hang this task.
                    rmt_tx_wait_all_done(static_cast<rmt_channel_handle_t>(channel_),
                                         pdMS_TO_TICKS(100));
                    shown = rgb;
                }
                // A failed write is simply retried next tick. FR-IND-6: a
                // dropped update is of no consequence, and there is nobody to
                // report it to who is not already looking at this pixel.
            }

            vTaskDelayUntil(&last, kTick);
        }
        // Last act while the handles are still safe to use: after this store
        // deinit() may free them at any instant.
        exited_.store(true, std::memory_order_release);
        vTaskDelete(nullptr);
    }
} // namespace ooe::pinled
