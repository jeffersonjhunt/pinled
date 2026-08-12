/**
 * @file pinled_live.cpp
 * @brief The scan → push hand-off for live monitoring.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "pinled_live.h"

namespace ooe::pinled
{
    size_t LiveState::snapshot(uint8_t *out, size_t cap)
    {
        if (out == nullptr || count == 0)
            return 0;

        const size_t needed = count * kLiveBytesPerChannel;
        if (cap < needed)
            return 0;

        // Drained in one pass, BEFORE any per-channel work, so the window each
        // bit describes ends at the same instant for every channel. Draining
        // inside the loop would make channel 0's window shorter than channel
        // 127's by however long the loop takes — small, but it would make two
        // lamps that flashed together look like they did not.
        uint32_t drained[kWords]{};
        for (size_t i = 0; i < kWords; ++i)
            drained[i] = active[i].exchange(0, std::memory_order_relaxed);

        for (size_t ch = 0; ch < count; ++ch)
        {
            uint8_t flags = 0;

            if (drained[ch / 32] & (1u << (ch % 32)))
                flags |= LIVE_ACTIVE;

            if (channels != nullptr && channels[ch].bound())
                flags |= LIVE_BOUND;

            if (classes != nullptr)
            {
                const uint8_t k = static_cast<uint8_t>(classes[ch]) & kLiveClassMask;
                flags = static_cast<uint8_t>(flags | (k << kLiveClassShift));
            }

            out[ch * kLiveBytesPerChannel] = levels != nullptr ? levels[ch] : 0;
            out[ch * kLiveBytesPerChannel + 1] = flags;
        }

        return needed;
    }
} // namespace ooe::pinled
