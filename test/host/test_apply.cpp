/**
 * @file test_apply.cpp
 * @brief Precedence between stored configuration and the profiler (FR-CFG-8).
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * Two sources set the same three numbers on every channel, so the only
 * interesting question is who wins. The case that matters most is the quiet
 * one: a lamp given an explicit fade must keep it across a boot-time profiling
 * pass. Get that wrong and per-lamp presentation appears to work, then stops at
 * the next reset — the kind of bug that gets reported as "it forgets".
 */

#include "harness.h"

#include "pinled_apply.h"

#include <cmath>
#include <cstring>

using namespace ooe::pinled;

namespace
{
    bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }

    /// What the classifier might return for a matrixed lamp.
    FilamentParams profiler_says(float attack = 55.0f, float decay = 65.0f, float gain = 8.0f)
    {
        FilamentParams p{};
        p.attack_ms = attack;
        p.decay_ms = decay;
        p.gain = gain;
        return p;
    }

    ChannelConfig styled(uint16_t attack, uint16_t decay, uint16_t gain, uint8_t flags,
                         DriveClass lock = DriveClass::UNKNOWN)
    {
        ChannelConfig c{};
        c.lamp = 17;
        c.led_index = 0;
        c.attack_ms = attack;
        c.decay_ms = decay;
        c.gain_permille = gain;
        c.flags = flags;
        c.class_lock = lock;
        return c;
    }
} // namespace

// --------------------------------------------------------- config -> params --

TEST(params_from_config_converts_units)
{
    ChannelConfig c{};
    c.attack_ms = 25;
    c.decay_ms = 45;
    c.gain_permille = 1500;

    const FilamentParams p = params_from_config(c);
    CHECK(near(p.attack_ms, 25.0f));
    CHECK(near(p.decay_ms, 45.0f));
    CHECK(near(p.gain, 1.5f)); // permille, so 1500 is 1.5x
}

TEST(unity_gain_is_one_thousand_permille)
{
    ChannelConfig c{};
    c.gain_permille = 1000;
    CHECK(near(params_from_config(c).gain, 1.0f));
}

TEST(a_zero_tau_is_floored_not_passed_through)
{
    // resolve() guarantees non-zero, but default_channels() and hand-built
    // records reach here too, and a zero tau makes the Q16 coefficient
    // degenerate. Enforced rather than assumed.
    ChannelConfig c{};
    c.attack_ms = 0;
    c.decay_ms = 0;
    const FilamentParams p = params_from_config(c);
    CHECK(p.attack_ms > 0.0f);
    CHECK(p.decay_ms > 0.0f);
}

TEST(a_zero_gain_becomes_unity_not_silence)
{
    // Gain 0 would mute the channel entirely. Unset means unity.
    ChannelConfig c{};
    c.gain_permille = 0;
    CHECK(near(params_from_config(c).gain, 1.0f));
}

// ------------------------------------------------------------- precedence --

TEST(an_unlocked_unstyled_channel_takes_the_profiler_wholesale)
{
    // The default path, and the reason the product does not ship a hand-tuned
    // table per game.
    const ChannelConfig c = styled(30, 40, 1000, CH_NONE);
    const FilamentParams p = effective_params(c, profiler_says());
    CHECK(near(p.attack_ms, 55.0f));
    CHECK(near(p.decay_ms, 65.0f));
    CHECK(near(p.gain, 8.0f));
}

TEST(a_locked_channel_ignores_the_profiler)
{
    const ChannelConfig c = styled(20, 30, 1200, CH_NONE, DriveClass::AC_DIMMED);
    const FilamentParams p = effective_params(c, profiler_says());
    CHECK(near(p.attack_ms, 20.0f));
    CHECK(near(p.decay_ms, 30.0f));
    CHECK(near(p.gain, 1.2f));
}

TEST(an_explicit_fade_survives_profiling_without_a_lock)
{
    // THE case this file exists for. Someone gave one insert a slow decay and
    // did not lock its class. The profiler still classifies the channel, but
    // must not take the decay back.
    const ChannelConfig c = styled(30, 120, 1000, CH_DECAY_SET);
    const FilamentParams p = effective_params(c, profiler_says());

    CHECK(near(p.decay_ms, 120.0f)); // the person's choice
    CHECK(near(p.attack_ms, 55.0f)); // still the profiler's
    CHECK(near(p.gain, 8.0f));       // still the profiler's
}

TEST(each_explicit_value_is_independent)
{
    const ChannelConfig c = styled(11, 22, 1300, CH_ATTACK_SET | CH_GAIN_SET);
    const FilamentParams p = effective_params(c, profiler_says());
    CHECK(near(p.attack_ms, 11.0f)); // chosen
    CHECK(near(p.decay_ms, 65.0f));  // inherited, so the profiler keeps it
    CHECK(near(p.gain, 1.3f));       // chosen
}

TEST(explicit_values_win_on_a_locked_channel_too)
{
    // The lock governs classification, not the user's own overrides — so the
    // two mechanisms compose rather than one shadowing the other.
    const ChannelConfig c = styled(11, 22, 1300, CH_DECAY_SET, DriveClass::STEADY);
    const FilamentParams p = effective_params(c, profiler_says());
    CHECK(near(p.attack_ms, 11.0f)); // from config, because locked
    CHECK(near(p.decay_ms, 22.0f));  // from config, explicitly
    CHECK(near(p.gain, 1.3f));
}

TEST(profiler_may_classify_tracks_the_lock)
{
    CHECK(profiler_may_classify(styled(1, 1, 1, CH_NONE, DriveClass::UNKNOWN)));
    const DriveClass locks[] = {DriveClass::OFF, DriveClass::STEADY, DriveClass::MATRIX,
                                DriveClass::AC_STEADY, DriveClass::AC_DIMMED};
    for (DriveClass l : locks)
        CHECK(!profiler_may_classify(styled(1, 1, 1, CH_NONE, l)));
}

TEST(setting_a_flag_without_a_value_still_yields_a_usable_tau)
{
    // A malformed record — flag set, value zero — must not produce a
    // degenerate coefficient.
    const ChannelConfig c = styled(0, 0, 0, CH_ATTACK_SET | CH_DECAY_SET | CH_GAIN_SET);
    const FilamentParams p = effective_params(c, profiler_says());
    CHECK(p.attack_ms > 0.0f);
    CHECK(p.decay_ms > 0.0f);
    CHECK(near(p.gain, 1.0f));
}

// ---------------------------------------------------------------- defaults --

TEST(default_channels_maps_identity_and_leaves_nothing_bound)
{
    // Must match what LampMap::set_default_mapping() and set_params_all()
    // already produced, or routing boot through the config path would change
    // behaviour on a rig that is already characterised.
    ChannelConfig ch[32];
    default_channels(ch, 32, 32);

    for (size_t i = 0; i < 32; ++i)
    {
        CHECK_EQ(ch[i].led_index, static_cast<int16_t>(i));
        CHECK(!ch[i].bound());
        CHECK(ch[i].mapped());
        CHECK(ch[i].auto_class());
        CHECK_EQ(ch[i].flags, static_cast<uint8_t>(CH_NONE));
    }
}

TEST(the_default_tint_is_the_one_lamp_map_already_used)
{
    // Found on hardware, not in a test: LampMap::set_default_mapping() had its
    // own copy of "warm white" as three literals (255,200,140) and this struct
    // had drifted to a different one. Routing boot through the config path
    // would have silently restyled every unconfigured lamp on a rig that had
    // been running for days.
    //
    // lamp_map.cpp now reads ResolveDefaults instead of keeping a copy, and
    // these values are asserted so the pair cannot drift apart again.
    const ResolveDefaults d;
    CHECK_EQ(d.r, static_cast<uint8_t>(255));
    CHECK_EQ(d.g, static_cast<uint8_t>(200));
    CHECK_EQ(d.b, static_cast<uint8_t>(140));

    ChannelConfig ch[4];
    default_channels(ch, 4, 4);
    CHECK_EQ(ch[0].r, d.r);
    CHECK_EQ(ch[0].g, d.g);
    CHECK_EQ(ch[0].b, d.b);
}

TEST(default_channels_leaves_channels_past_the_string_unmapped)
{
    // 32 channels, 16 LEDs: the back half has nowhere to render.
    ChannelConfig ch[32];
    default_channels(ch, 32, 16);

    CHECK(ch[15].mapped());
    CHECK(!ch[16].mapped());
    CHECK_EQ(ch[31].led_index, static_cast<int16_t>(-1));
}

TEST(default_channels_uses_the_supplied_defaults)
{
    ResolveDefaults d{};
    d.attack_ms = 12;
    d.decay_ms = 34;
    d.r = 1;
    d.g = 2;
    d.b = 3;

    ChannelConfig ch[4];
    default_channels(ch, 4, 4, d);
    CHECK_EQ(ch[0].attack_ms, static_cast<uint16_t>(12));
    CHECK_EQ(ch[0].decay_ms, static_cast<uint16_t>(34));
    CHECK_EQ(ch[0].g, static_cast<uint8_t>(2));
}

TEST(default_channels_leaves_the_profiler_in_charge_of_everything)
{
    // No flags set, so a boot-time pass may replace all of it — which is what
    // makes the pre-configuration behaviour identical to before.
    ChannelConfig ch[8];
    default_channels(ch, 8, 8);
    for (size_t i = 0; i < 8; ++i)
    {
        const FilamentParams p = effective_params(ch[i], profiler_says());
        CHECK(near(p.attack_ms, 55.0f));
        CHECK(near(p.decay_ms, 65.0f));
        CHECK(near(p.gain, 8.0f));
    }
}

TEST(default_channels_tolerates_a_null_array)
{
    default_channels(nullptr, 8, 8); // must not crash
    CHECK(true);
}

int main() { return ooe::test::run_all(); }
