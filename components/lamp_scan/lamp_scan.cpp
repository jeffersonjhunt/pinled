/**
 * @file lamp_scan.cpp
 * @brief Chained 74x165 scan driver over SPI + DMA (rev D).
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "lamp_scan.h"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h" // esp_rom_delay_us
#include "sdkconfig.h"

// The slow-step and hold-channel diagnostics need per-edge control of CLK and
// of /PL, neither of which the SPI peripheral gives us. When either is compiled
// in, fall back to the bit-bang backend. Unlike rev B this is a driver
// limitation only -- the '165 chain itself is happy to be clocked at any speed
// or stopped indefinitely, so these modes work on production hardware too.
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
        if (cfg.spi_mode > 3)
            return ESP_ERR_INVALID_ARG;

        deinit();
        cfg_ = cfg;

        // When /PL is driven from SPI CS the peripheral owns the pin, so only
        // configure it as a GPIO for the bit-bang path or when CS is not used.
        pl_is_gpio_ = (cfg_.pl_pin != GPIO_NUM_NC) &&
                      (LAMPSCAN_BITBANG || !cfg_.pl_from_cs);
        if (pl_is_gpio_)
        {
            gpio_reset_pin(cfg_.pl_pin);
            gpio_set_direction(cfg_.pl_pin, GPIO_MODE_OUTPUT);
            // Idle LOW: registers transparent, tracking their inputs. This is
            // the opposite of rev C's /MR, which idled high. Idling high here
            // would leave the chain frozen on whatever it held at boot and
            // shift the terminator's zeros in behind it.
            gpio_set_level(cfg_.pl_pin, 0);
        }

        const esp_err_t err = LAMPSCAN_BITBANG ? init_bitbang() : init_spi();
        if (err != ESP_OK)
        {
            deinit();
            return err;
        }

        initialized_ = true;

        ESP_LOGI(TAG, "init: %u module(s) x %u ch, %u clocks/frame (%u bytes), active_%s, %s",
                 (unsigned)cfg_.num_modules, (unsigned)cfg_.channels_per_module,
                 (unsigned)frame_bits(), (unsigned)frame_bytes(),
                 cfg_.active_low ? "low" : "high",
                 LAMPSCAN_BITBANG ? "bit-bang (diagnostic build)" : "SPI+DMA");
        if (!LAMPSCAN_BITBANG)
        {
            ESP_LOGI(TAG, "  SPI host %d, %d Hz, mode %u, SCLK=%d MISO=%d, /PL=%d (%s)",
                     (int)cfg_.spi_host, cfg_.spi_hz, (unsigned)cfg_.spi_mode,
                     (int)cfg_.clk_pin, (int)cfg_.data_pin, (int)cfg_.pl_pin,
                     (cfg_.pl_from_cs && cfg_.pl_pin != GPIO_NUM_NC) ? "SPI CS" : "GPIO");
            if (cfg_.spi_mode != 2)
                ESP_LOGW(TAG, "  mode %u is wrong for a '165 chain: mode 2 is "
                              "measured, not assumed. Expect a whole-frame "
                              "off-by-one and an unreachable channel 0",
                         (unsigned)cfg_.spi_mode);
        }
        return ESP_OK;
    }

    esp_err_t LampScan::init_spi()
    {
        const size_t bytes = frame_bytes();

        // Must be DMA-capable. 16 bytes at the 128-channel maximum, so this
        // also fits the SPI FIFO outright — the burst is gapless because the
        // clock comes out of the peripheral, not because DMA keeps up.
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
        // CS drives /PL. Positive polarity puts it LOW between transactions
        // (registers transparent, loading) and HIGH during one (frozen,
        // shifting), so the capture instant is hardware-timed. cs_ena_pretrans
        // is the CS-rise-to-first-clock-edge margin in SPI bit times: it is
        // what guarantees the load has actually settled before the first shift,
        // and it is why channel 0 needs no clock of its own.
        if (cfg_.pl_from_cs && cfg_.pl_pin != GPIO_NUM_NC)
        {
            dev.spics_io_num = cfg_.pl_pin;
            dev.flags |= SPI_DEVICE_POSITIVE_CS;
            dev.cs_ena_pretrans = 2;
            dev.cs_ena_posttrans = 2;
        }
        else
        {
            dev.spics_io_num = -1;
        }
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
        // Idle HIGH, matching mode 2's CPOL=1 so the bit-bang path presents the
        // same edge sequence as the SPI path. step_clock() then pulses low and
        // back, and it is the trailing rising edge that shifts.
        gpio_set_level(cfg_.clk_pin, 1);

        // gpio_reset_pin() leaves the internal pull-UP enabled and
        // gpio_set_direction() does not clear it. That fights the chain
        // terminator and, with the non-inverting front end, makes an undriven
        // DATA line read "lamp on" -- the opposite of HW-2/HW-11. Bias follows
        // polarity: the two are one decision.
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
        dev.queue_size = 1;
        if (cfg_.pl_from_cs && cfg_.pl_pin != GPIO_NUM_NC)
        {
            dev.spics_io_num = cfg_.pl_pin;
            dev.flags |= SPI_DEVICE_POSITIVE_CS;
            dev.cs_ena_pretrans = 2;
            dev.cs_ena_posttrans = 2;
        }
        else
        {
            dev.spics_io_num = -1;
        }

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

    void LampScan::reload_chain()
    {
        // /PL is level-sensitive: low makes every register transparent, and the
        // rising edge freezes the whole chain at one instant. QH then already
        // holds channel 0, so the first bit costs no clock. No-op when CS is
        // driving /PL -- the peripheral does exactly this around every
        // transaction, and more precisely than software can.
        if (!pl_is_gpio_)
            return;
        gpio_set_level(cfg_.pl_pin, 0);
        esp_rom_delay_us(1); // coarse load-settle; datasheet wants nanoseconds
        gpio_set_level(cfg_.pl_pin, 1);
    }

    void LampScan::hold_transparent()
    {
        if (!pl_is_gpio_)
            return;
        gpio_set_level(cfg_.pl_pin, 0);
    }

    esp_err_t LampScan::read_frame(bool *out, size_t n)
    {
        if (!initialized_)
            return ESP_ERR_INVALID_STATE;
        const size_t total = total_channels();
        if (!out || n < total)
            return ESP_ERR_INVALID_ARG;

#if LAMPSCAN_BITBANG
        reload_chain(); // snapshot; QH now presents channel 0 with no clock
        for (size_t ch = 0; ch < total; ++ch)
        {
            esp_rom_delay_us(1); // coarse settle; this path is diagnostic only
            out[ch] = sample_now(0);
            step_clock(); // rising edge advances to channel ch+1
        }
        hold_transparent(); // back to the idle state: registers tracking
        return ESP_OK;
#else
        reload_chain(); // no-op when CS drives /PL

        const size_t bits = frame_bytes() * 8;
        spi_transaction_t t{};
        t.length = bits;       // exactly 16*N; both channel counts are whole bytes
        t.rxlength = bits;     // bits captured on MISO
        t.tx_buffer = nullptr; // MOSI unused
        t.rx_buffer = rx_;

        // Polling rather than interrupt-driven: the block-and-wake round trip
        // costs ~37 us per transaction, which dwarfs the burst itself and does
        // not fit a 100 us period at 128 channels. Polling busy-waits, so it
        // trades CPU for latency -- see docs/TIMING.md section 2.4.
        const esp_err_t err = spi_device_polling_transmit(spi_, &t);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "spi_device_polling_transmit: %s", esp_err_to_name(err));
            return err;
        }

        // MSB-first, one bit per channel, nothing discarded: stream bit i is
        // channel i (FR-SCAN-4). The stream is module-major because U1 sits at
        // the head of each module's pair and each module sits ahead of the next.
        for (size_t ch = 0; ch < total; ++ch)
        {
            const bool bit = (rx_[ch / 8] >> (7 - (ch % 8))) & 1;
            out[ch] = cfg_.active_low ? !bit : bit;
        }
        return ESP_OK;
#endif
    }

    void LampScan::step_clock()
    {
#if LAMPSCAN_BITBANG
        if (!initialized_)
            return;
        // Idle high (CPOL=1). The trailing rising edge is the one that shifts,
        // so callers must sample BEFORE calling this, not after.
        gpio_set_level(cfg_.clk_pin, 0);
        gpio_set_level(cfg_.clk_pin, 1);
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
