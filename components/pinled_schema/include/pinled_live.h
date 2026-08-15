#pragma once

/**
 * @file pinled_live.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief The scan → push hand-off for live monitoring (FR-UI-5/9).
 * @version 0.6.0
 * @date 2026-08-11
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * @par The problem this solves
 * `scan_task` runs at 10 kHz on core 1 at near-top priority. The push task
 * runs at ~30 Hz, low priority, on core 0, and **must not be able to slow the
 * scan down** (FR-UI-9) — the scan is the product; the monitor is a
 * convenience. So the hand-off has to be lock-free in the scan direction and
 * must lose nothing when a push is late or dropped.
 *
 * @par Sticky activity, and why it makes a dropped push free
 * A lamp in a matrix is on for a few hundred microseconds. Sampling it at
 * 30 Hz would miss it almost always, and the UI would show a dark playfield
 * on a working machine. So the scan does not publish instants — it publishes
 * a **sticky** "this channel was active at some point since you last looked",
 * and the push **clears it as it reads** (FR-UI-5).
 *
 * That single property is what lets FR-UI-9 be true: a dropped push is not a
 * lost sample, it is a longer window. Nothing accumulates, nothing blocks, and
 * the scan never waits for a reader.
 *
 * @par Why a bitmap and not a byte per channel
 * The obvious shape is one flag per channel, but that costs the scan an atomic
 * read-modify-write per active channel per frame — up to 128 × 10 000 a second.
 * Instead the scan accumulates into a plain local word set (no atomics at all
 * in the inner loop) and publishes once per frame with four `fetch_or`s. Same
 * information, 40 000 atomics a second instead of 1.28 million.
 *
 * @par The one ordering subtlety
 * Publishing with `fetch_or` and draining with `exchange(0)` is what makes the
 * race benign. Both are read-modify-write operations on the same word, so they
 * serialise: a set that lands before the drain is reported now, one that lands
 * after survives to the next push. A plain load-then-store on the drain side
 * would silently swallow anything the scan set in between — which would look
 * like "the UI occasionally misses a flash" and be near-impossible to chase.
 * `relaxed` is sufficient: these bits carry no other data with them.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "pinled_channel_config.h"
#include "pinled_drive_class.h"

namespace ooe::pinled
{
    /// Bytes per channel in the packed payload. See `LiveState::snapshot`.
    inline constexpr size_t kLiveBytesPerChannel = 2;

    /// Flags in the second byte of each channel's sample.
    enum LiveFlags : uint8_t
    {
        LIVE_ACTIVE = 1u << 0, ///< active at least once since the last push
        LIVE_BOUND = 1u << 1,  ///< channel is bound to a lamp number
        // bits 2..4 carry DriveClass; bits 5..7 are reserved and sent as 0.
    };

    inline constexpr uint8_t kLiveClassShift = 2;
    inline constexpr uint8_t kLiveClassMask = 0x07;

    /**
     * @brief Shared state between the scan task and the push task.
     *
     * The pointers are borrowed, not owned: they refer to buffers that live for
     * the lifetime of the application. Only `active` is written from two
     * threads.
     */
    struct LiveState
    {
        static constexpr size_t kMaxChannels = 128;
        static constexpr size_t kWords = kMaxChannels / 32;

        /// Sticky activity bitmap. Set by the scan, cleared by the push.
        std::atomic<uint32_t> active[kWords]{};

        const uint8_t *levels{nullptr};        ///< reconstructed brightness, 0..255
        const ChannelConfig *channels{nullptr};///< for the bound flag
        const DriveClass *classes{nullptr};    ///< as last classified
        size_t count{0};

        /**
         * @brief Publish one frame's activity. Called from the scan task.
         *
         * @param bits `kWords` words, bit *n* set when channel *n* was active
         *             in this frame. Accumulated by the caller without atomics.
         */
        void publish(const uint32_t *bits)
        {
            for (size_t i = 0; i < kWords; ++i)
                if (bits[i] != 0)
                    active[i].fetch_or(bits[i], std::memory_order_relaxed);
        }

        /**
         * @brief Drain into the packed wire payload. Called from the push task.
         *
         * Two bytes per channel, channel-major:
         * - `[0]` level, 0..255, post-filament
         * - `[1]` `LiveFlags`, with DriveClass in bits 2..4
         *
         * @param out destination
         * @param cap capacity in bytes
         * @return bytes written, or 0 if @p cap cannot hold `count` channels.
         *
         * @note Clears the sticky bits as it reads them — calling this twice
         *       reports activity once, which is the point.
         */
        size_t snapshot(uint8_t *out, size_t cap);

        /**
         * @brief Clear the sticky bits without reporting them.
         *
         * For the push task while nobody is connected. "Sticky since the last
         * push" quietly becomes "sticky since the last *reader*" if nothing
         * drains it, because the scan publishes whether or not anyone is
         * listening — so the first frame a connecting client receives would
         * carry every blip since the previous client left. Observed on the
         * bench: one transient on channel 20 survived several minutes between
         * sessions and arrived looking live.
         *
         * The window is meant to be one push long. This keeps it that way when
         * there is nobody to push to.
         */
        void drain()
        {
            for (size_t i = 0; i < kWords; ++i)
                active[i].store(0, std::memory_order_relaxed);
        }
    };
} // namespace ooe::pinled
