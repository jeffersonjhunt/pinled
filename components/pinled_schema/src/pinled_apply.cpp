/**
 * @file pinled_apply.cpp
 * @brief Precedence between stored configuration and the profiler.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "pinled_apply.h"

namespace ooe::pinled
{
    namespace
    {
        constexpr float kPermille = 1000.0f;

        /// A tau of zero would make the integrator coefficient degenerate.
        /// resolve() guarantees non-zero, but default_channels() and hand-built
        /// records reach here too, so the floor is enforced rather than assumed.
        constexpr uint16_t kMinTauMs = 1;

        float tau(uint16_t ms)
        {
            return static_cast<float>(ms < kMinTauMs ? kMinTauMs : ms);
        }
    } // namespace

    FilamentParams params_from_config(const ChannelConfig &c)
    {
        FilamentParams p{};
        p.attack_ms = tau(c.attack_ms);
        p.decay_ms = tau(c.decay_ms);
        p.gain = c.gain_permille != 0 ? static_cast<float>(c.gain_permille) / kPermille
                                      : 1.0f;
        return p;
    }

    bool profiler_may_classify(const ChannelConfig &c)
    {
        return c.auto_class();
    }

    FilamentParams effective_params(const ChannelConfig &c,
                                    const FilamentParams &from_profiler)
    {
        // Start from whichever source owns the channel's classification.
        FilamentParams p = profiler_may_classify(c) ? from_profiler
                                                    : params_from_config(c);

        // Then let explicit per-lamp choices win regardless. This is the half
        // that makes "give this insert a softer fade than the bulb had" survive
        // a boot-time profiling pass — without it the feature works until the
        // next reset and then quietly stops.
        const FilamentParams chosen = params_from_config(c);
        if (c.has(CH_ATTACK_SET))
            p.attack_ms = chosen.attack_ms;
        if (c.has(CH_DECAY_SET))
            p.decay_ms = chosen.decay_ms;
        if (c.has(CH_GAIN_SET))
            p.gain = chosen.gain;

        return p;
    }

    void default_channels(ChannelConfig *out, size_t n, size_t led_count,
                          const ResolveDefaults &d)
    {
        if (out == nullptr)
            return;

        for (size_t i = 0; i < n; ++i)
        {
            ChannelConfig &c = out[i];
            c = ChannelConfig{};
            c.lamp = 0; // nothing is bound until a document says so
            c.led_index = i < led_count ? static_cast<int16_t>(i) : static_cast<int16_t>(-1);
            c.r = d.r;
            c.g = d.g;
            c.b = d.b;
            c.attack_ms = d.attack_ms;
            c.decay_ms = d.decay_ms;
            c.gain_permille = d.gain_permille;
            // flags stay CH_NONE: none of this was chosen by anyone, so the
            // profiler is free to replace all of it.
            c.flags = CH_NONE;
        }
    }
} // namespace ooe::pinled
