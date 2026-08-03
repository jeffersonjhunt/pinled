/**
 * @file lamp_scan.cpp
 * @brief Chained '161 + dual '251 scan driver over SPI + DMA.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "lamp_scan.h"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h" // esp_rom_delay_us
#include "sdkconfig.h"

// The slow-step and hold-channel diagnostics need per-edge control of CLK, which
// the SPI peripheral does not give us. When either is compiled in, fall back to
// the bit-bang backend. Both are bench-rig only anyway (FR-DIAG-2/3): a
// production module releases its bus grant if the clock stops.
#if CONFIG_PINLED_SCAN_STEP_MS > 0 || CONFIG_PINLED_SCAN_HOLD_CH >= 0
#define LAMPSCAN_BITBANG 1
#else
#define LAMPSCAN_BITBANG 0
#endif

namespace ooe::pinled
{
    static const char *TAG = "lamp_scan";

    esp_err_t LampScan::init(const LampScanConfig &cfg)
    {
        if (cfg.clk_pin == GPIO_NUM_NC || cfg.data_pin == GPIO_NUM_NC)
            return ESP_ERR_INVALID_ARG;
        if (cfg.num_modules == 0 || cfg.num_modules > LAMPSCAN_MAX_MODULES)
            return ESP_ERR_INVALID_ARG;
        if (cfg.channels_per_module != 8 && cfg.channels_per_module != 16)
            return ESP_ERR_INVALID_ARG;
        if (cfg.spi_mode > 1)
            return ESP_ERR_INVALID_ARG;

        deinit();
        cfg_ = cfg;

        // `/MR` is a plain GPIO in both backends. Production modules leave it
        // unset and reset on the clock-idle timeout instead.
        if (cfg_.mr_pin != GPIO_NUM_NC)
        {
            gpio_reset_pin(cfg_.mr_pin);
            gpio_set_direction(cfg_.mr_pin, GPIO_MODE_OUTPUT);
            gpio_set_level(cfg_.mr_pin, 1); // idle high (not clearing)
        }

        const esp_err_t err = LAMPSCAN_BITBANG ? init_bitbang() : init_spi();
        if (err != ESP_OK)
        {
            deinit();
            return err;
        }

        initialized_ = true;
        reset_counter();

        ESP_LOGI(TAG, "init: %u module(s) x %u ch = %u bits/frame, active_%s, %s",
                 (unsigned)cfg_.num_modules, (unsigned)cfg_.channels_per_module,
                 (unsigned)frame_bits(), cfg_.active_low ? "low" : "high",
                 LAMPSCAN_BITBANG ? "bit-bang (diagnostic build)" : "SPI+DMA");
        if (!LAMPSCAN_BITBANG)
            ESP_LOGI(TAG, "  SPI host %d, %d Hz, mode %u, SCLK=%d MISO=%d, /MR=%d",
                     (int)cfg_.spi_host, cfg_.spi_hz, (unsigned)cfg_.spi_mode,
                     (int)cfg_.clk_pin, (int)cfg_.data_pin, (int)cfg_.mr_pin);
        return ESP_OK;
    }

    esp_err_t LampScan::init_spi()
    {
        const size_t bytes = frame_bits() / 8;

        // Must be DMA-capable. 16 bytes at the 128-channel maximum, so this
        // also fits the SPI FIFO outright — the burst is gapless because the
        // shift register is hardware-clocked, not because DMA keeps up.
        rx_ = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_DMA));
        if (!rx_)
            return ESP_ERR_NO_MEM;

        spi_bus_config_t bus{};
        bus.mosi_io_num = -1; // nothing to send; the chain is read-only
        bus.miso_io_num = cfg_.data_pin;
        bus.sclk_io_num = cfg_.clk_pin;
        bus.quadwp_io_num = -1;
        bus.quadhd_io_num = -1;
        bus.max_transfer_sz = static_cast<int>(bytes);

        esp_err_t err = spi_bus_initialize(cfg_.spi_host, &bus, SPI_DMA_CH_AUTO);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
            return err;
        }
        bus_owned_ = true;

        spi_device_interface_config_t dev{};
        dev.clock_speed_hz = cfg_.spi_hz;
        dev.mode = cfg_.spi_mode;
        dev.spics_io_num = -1; // no chip select; the chain has none
        dev.queue_size = 1;

        err = spi_bus_add_device(cfg_.spi_host, &dev, &spi_);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "spi_bus_add_device: %s", esp_err_to_name(err));
            return err;
        }

        // Note: this reports kHz, not Hz.
        int actual_khz = 0;
        if (spi_device_get_actual_freq(spi_, &actual_khz) == ESP_OK)
        {
            const int actual_hz = actual_khz * 1000;
            if (actual_hz != cfg_.spi_hz)
                ESP_LOGW(TAG, "SPI clock is %d Hz, not the requested %d (divider rounding)",
                         actual_hz, cfg_.spi_hz);
            cfg_.spi_hz = actual_hz; // report what the hardware actually does
        }

        return ESP_OK;
    }

    esp_err_t LampScan::init_bitbang()
    {
        gpio_reset_pin(cfg_.clk_pin);
        gpio_set_direction(cfg_.clk_pin, GPIO_MODE_OUTPUT);
        gpio_set_level(cfg_.clk_pin, 0);

        // gpio_reset_pin() leaves the internal pull-UP enabled and
        // gpio_set_direction() does not clear it. That fights the master's
        // 1 kOhm bias and, with the non-inverting front end, makes an undriven
        // bus read "lamp on" -- the opposite of HW-2. Bias follows polarity:
        // the two are one decision.
        gpio_config_t io{};
        io.pin_bit_mask = 1ULL << static_cast<uint32_t>(cfg_.data_pin);
        io.mode = GPIO_MODE_INPUT;
        io.pull_up_en = cfg_.active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
        io.pull_down_en = cfg_.active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
        io.intr_type = GPIO_INTR_DISABLE;
        return gpio_config(&io);
    }

    esp_err_t LampScan::set_clock(int hz)
    {
#if LAMPSCAN_BITBANG
        (void)hz;
        return ESP_ERR_NOT_SUPPORTED;
#else
        if (!initialized_ || !spi_)
            return ESP_ERR_INVALID_STATE;

        spi_bus_remove_device(spi_);
        spi_ = nullptr;

        spi_device_interface_config_t dev{};
        dev.clock_speed_hz = hz;
        dev.mode = cfg_.spi_mode;
        dev.spics_io_num = -1;
        dev.queue_size = 1;

        const esp_err_t err = spi_bus_add_device(cfg_.spi_host, &dev, &spi_);
        if (err != ESP_OK)
            return err;

        int actual_khz = 0;
        cfg_.spi_hz = (spi_device_get_actual_freq(spi_, &actual_khz) == ESP_OK)
                          ? actual_khz * 1000
                          : hz;
        return ESP_OK;
#endif
    }

    void LampScan::deinit()
    {
        if (spi_)
        {
            spi_bus_remove_device(spi_);
            spi_ = nullptr;
        }
        if (bus_owned_)
        {
            spi_bus_free(cfg_.spi_host);
            bus_owned_ = false;
        }
        if (rx_)
        {
            heap_caps_free(rx_);
            rx_ = nullptr;
        }
        initialized_ = false;
    }

    void LampScan::reset_counter()
    {
        // '161 `/MR` is asynchronous: a low pulse zeroes Q0..Q3 immediately.
        // Production modules have no such wire — the inter-frame clock idle
        // discharges the activity detector and clears the chain instead.
        if (cfg_.mr_pin == GPIO_NUM_NC)
            return;
        gpio_set_level(cfg_.mr_pin, 0);
        esp_rom_delay_us(1);
        gpio_set_level(cfg_.mr_pin, 1);
    }

    esp_err_t LampScan::read_frame(bool *out, size_t n)
    {
        if (!initialized_)
            return ESP_ERR_INVALID_STATE;
        const size_t total = total_channels();
        if (!out || n < total)
            return ESP_ERR_INVALID_ARG;

#if LAMPSCAN_BITBANG
        reset_counter();
        for (size_t ch = 0; ch < total; ++ch)
        {
            esp_rom_delay_us(1); // coarse settle; this path is bench-rig only
            out[ch] = sample_now(0);
            step_counter();
        }
        return ESP_OK;
#else
        reset_counter(); // no-op on production hardware

        spi_transaction_t t{};
        t.length = frame_bits();   // clocks generated
        t.rxlength = frame_bits(); // bits captured on MISO
        t.tx_buffer = nullptr;     // MOSI unused
        t.rx_buffer = rx_;

        const esp_err_t err = spi_device_transmit(spi_, &t);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "spi_device_transmit: %s", esp_err_to_name(err));
            return err;
        }

        // MSB-first: sample 1 (line 0 of module 1) is the top bit of rx_[0].
        for (size_t ch = 0; ch < total; ++ch)
        {
            const bool bit = (rx_[ch / 8] >> (7 - (ch % 8))) & 1;
            out[ch] = cfg_.active_low ? !bit : bit;
        }
        return ESP_OK;
#endif
    }

    void LampScan::step_counter()
    {
#if LAMPSCAN_BITBANG
        if (!initialized_)
            return;
        gpio_set_level(cfg_.clk_pin, 1);
        gpio_set_level(cfg_.clk_pin, 0);
#endif
    }

    bool LampScan::sample_now(size_t module)
    {
        (void)module;
#if LAMPSCAN_BITBANG
        if (!initialized_)
            return false;
        const int raw = gpio_get_level(cfg_.data_pin);
        return cfg_.active_low ? (raw == 0) : (raw != 0);
#else
        return false;
#endif
    }
} // namespace ooe::pinled
