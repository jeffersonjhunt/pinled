#pragma once

/**
 * @file lamp_scan.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief Chained 74x165 lamp-channel scan driver, rev D (SPI + DMA).
 * @version 0.4.0
 * @date 2026-08-06
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * Modules are identical and unaddressed. Each carries two 74x165 parallel-in /
 * serial-out shift registers: `U1` holds channels 0..7, `U2` holds 8..15 and
 * feeds `U1`'s `SER`. `U1`'s `QH` leaves the module toward the master, and its
 * own `SER` chain continues to the next module downstream. `CLK` and `/PL` are
 * bussed to every register; `DATA` is point-to-point, one push-pull driver per
 * hop. See docs/CHAINING.md.
 *
 * @par The frame
 * `/PL` is **level**-sensitive, not edge-triggered: while low every register is
 * transparent and tracks its inputs continuously, and the rising edge freezes
 * the entire chain at once. Every channel in the system therefore shares a
 * single capture instant, however long the chain is.
 *
 * After that edge `QH` already presents channel 0 -- no clock is needed to
 * reach the first bit -- and each subsequent rising `CLK` edge advances the
 * stream by one channel. A frame is exactly **16 clocks per module**, with no
 * arm clock and nothing discarded: stream bit *i* is channel *i*, MSB-first.
 *
 * @par Why one transaction matters
 * The whole chain is read in a **single** SPI transaction. The driver's fixed
 * per-transaction overhead is ~17 us on this hardware, essentially independent
 * of clock rate, and paying it once per frame rather than once per module is
 * what keeps 128 channels inside the 10 kHz budget. See docs/TIMING.md §2.4.
 *
 * @par SPI mode (reasoned, NOT yet measured)
 * A '165 shifts `QH` on the **rising** clock edge. **Mode 2** (`CPOL=1,
 * CPHA=0`) idles the clock high and samples on the falling edge, which lands
 * mid bit-cell: half a period after the shift that produced the bit, half a
 * period before the next.
 *
 * Mode 3 samples on the same rising edge the register shifts on, so the frame
 * slides by one channel and channel 0 becomes unreachable. A clean whole-frame
 * off-by-one is that signature, and it is the most likely rev D bring-up fault.
 *
 * Unlike rev C, this is derived from the shift edge rather than measured -- no
 * '165 has been on the bench yet. It is bring-up check #1, docs/BRINGUP.md §5.
 * Rev C's inverter in the `CLK` path has no rev D counterpart: phase is handled
 * entirely by the SPI mode.
 *
 * @par Driving /PL
 * Preferably from the SPI `CS` line with positive polarity, so it is low
 * between transactions (transparent, loading) and high during one (frozen,
 * shifting) with no software involvement -- the capture instant is then
 * hardware-timed. Set `pl_from_cs` for that; otherwise the driver pulses
 * `pl_pin` as a plain GPIO before each frame and idles it low between them.
 */

#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"

namespace ooe::pinled
{
    static constexpr size_t LAMPSCAN_MAX_MODULES = 8;

    struct LampScanConfig
    {
        gpio_num_t clk_pin{GPIO_NUM_NC};  ///< chain `CLK`, driven as SPI `SCLK`
        gpio_num_t data_pin{GPIO_NUM_NC}; ///< serial `DATA` stream, read as SPI `MISO`
        gpio_num_t pl_pin{GPIO_NUM_NC};   ///< bussed '165 parallel load (active low, level)
        bool pl_from_cs{true};            ///< drive /PL from SPI CS (positive polarity)

        spi_host_device_t spi_host{SPI2_HOST};
        int spi_hz{2 * 1000 * 1000}; ///< chain clock; see docs/TIMING.md §4.3
        uint8_t spi_mode{2};         ///< 2 = sample mid bit-cell; see the file header

        size_t num_modules{1};          ///< modules on the chain (1..8)
        size_t channels_per_module{16}; ///< 8 (one '165) or 16 (two)
        bool active_low{false};         ///< MOSFET + inverting Schmitt = non-inverting (HW-1)
    };

    class LampScan
    {
    public:
        static constexpr size_t MAX_CHANNELS = LAMPSCAN_MAX_MODULES * 16;

        esp_err_t init(const LampScanConfig &cfg);
        void deinit();

        /**
         * @brief Read one full scan frame into out[].
         *
         * One SPI transaction clocks the entire chain; channel order is
         * module-major, so out[m * channels_per_module + ch].
         *
         * A `num_modules` that does not match the hardware degrades benignly in
         * both directions (FR-SCAN-11): too high and the surplus channels read
         * the terminator's hard zeros, too low and the trailing modules simply
         * go unread. Neither shifts the channels that are present.
         *
         * @param out  caller buffer of booleans (already polarity-corrected)
         * @param n    capacity of out; must be >= total_channels()
         */
        esp_err_t read_frame(bool *out, size_t n);

        size_t total_channels() const { return cfg_.num_modules * cfg_.channels_per_module; }

        /// Bits clocked per frame: exactly one per channel, no arm clocks and
        /// nothing discarded. Equal to total_channels() by construction, and
        /// kept separate only because the byte math below reads better for it.
        size_t frame_bits() const { return total_channels(); }

        /// Bytes actually clocked. Both legal channel counts (8 and 16) are
        /// whole bytes, so this never rounds up and a frame carries no surplus
        /// clocks -- unlike rev C, where the 17-clock stride usually did.
        size_t frame_bytes() const { return (frame_bits() + 7) / 8; }

        /// Re-open the SPI device at a new clock rate. Diagnostic use: lets a
        /// sweep find the chain's ceiling without a reflash per step. No-op on
        /// the bit-bang backend.
        esp_err_t set_clock(int hz);

        /// Clock the hardware actually produces, after divider rounding.
        int actual_hz() const { return cfg_.spi_hz; }

        /// Pulse `/PL` low then high: snapshot the whole chain and freeze it,
        /// leaving channel 0 on `DATA`. No-op when `CS` drives `/PL`, since the
        /// peripheral already does exactly this around every transaction.
        void reload_chain();

        /// Drive `/PL` low and leave it there, so the registers stay
        /// transparent and `DATA` tracks channel 0 live (FR-DIAG-3). Bench use;
        /// no-op when `CS` drives `/PL`.
        void hold_transparent();

        /**
         * @name Bring-up helpers
         * Step the chain one bit at a time so `DATA` can be watched or probed.
         * The chain holds no analog state, so it can be clocked arbitrarily
         * slowly or stopped outright -- on production hardware as well as a
         * bench rig. Only available when a diagnostic mode is compiled in,
         * which also selects the bit-bang backend instead of SPI.
         * @{
         */
        void step_clock();                  ///< one CLK pulse; shifts on its rising edge
        bool sample_now(size_t module = 0); ///< read `DATA` now, polarity-corrected
        /// @}

    private:
        esp_err_t init_spi();
        esp_err_t init_bitbang();

        LampScanConfig cfg_{};
        bool initialized_{false};
        bool pl_is_gpio_{false}; ///< /PL driven as a plain GPIO rather than by CS

        spi_device_handle_t spi_{nullptr};
        bool bus_owned_{false};
        uint8_t *rx_{nullptr}; ///< DMA-capable receive buffer, frame_bits()/8 bytes
    };
} // namespace ooe::pinled
