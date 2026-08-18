/**
 * @file test_indicator.cpp
 * @brief Status-pixel pattern generation (FR-IND-1..8).
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * The indicator is the only feedback a commissioned board has, and every one
 * of its patterns is a claim about something a person will read off it in a
 * dark backbox: how many blinks, which one wins when two are true at once,
 * whether releasing the button at 4.9 s stops the erase. Staging those on
 * hardware means holding a button and counting out loud; here it is stepping
 * a number.
 */

#include "harness.h"

#include "pinled_indicator.h"

#include <cmath>

using namespace ooe::pinled;

namespace
{
    // Named rather than written inline: a braced initialiser inside CHECK()
    // reads as three macro arguments.
    constexpr IndicatorRgb kOff{0, 0, 0};
    constexpr IndicatorRgb kFullWhite{255, 255, 255};

    /// Count rising edges of "lit" across a span, sampling at 10 ms — the rate
    /// the indicator task actually runs at, so the count is what the driver
    /// would produce rather than what continuous maths would.
    int count_blinks(IndicatorInput in, uint32_t from_ms, uint32_t to_ms,
                     uint32_t step_ms = 10)
    {
        int edges = 0;
        bool prev = false;
        for (uint32_t t = from_ms; t < to_ms; t += step_ms)
        {
            in.state_ms = t;
            const bool lit = indicator_render(in) != kOff;
            if (lit && !prev)
                ++edges;
            prev = lit;
        }
        return edges;
    }

    IndicatorInput running()
    {
        IndicatorInput in{};
        in.state = IndicatorState::RUNNING;
        in.brightness = 255;
        return in;
    }
} // namespace

// --- fault classes (FR-IND-4) -----------------------------------------------

TEST(fault_class_value_is_the_blink_count)
{
    // Not a coincidence and not a lookup table: the enum value IS the count,
    // which is what keeps the indicator and the documented table from drifting.
    CHECK_EQ(static_cast<int>(FaultClass::SENSE_BUS), 2);
    CHECK_EQ(static_cast<int>(FaultClass::CONFIG), 3);
    CHECK_EQ(static_cast<int>(FaultClass::LED_STRING), 4);
    CHECK_EQ(static_cast<int>(FaultClass::STORAGE), 5);
    CHECK_EQ(static_cast<int>(FaultClass::INTERNAL), 6);
}

TEST(counting_starts_at_two)
{
    // One blink and a pause reads as a heartbeat rather than as a count, so
    // no class may occupy 1.
    CHECK_EQ(static_cast<int>(kFaultClassMin), 2);
    CHECK_EQ(fault_bit(FaultClass::NONE), 0);
    CHECK_EQ(fault_bit(static_cast<FaultClass>(1)), 0);
    CHECK_EQ(fault_bit(static_cast<FaultClass>(7)), 0);
}

TEST(lowest_fault_wins_when_several_are_active)
{
    const uint8_t both = static_cast<uint8_t>(fault_bit(FaultClass::STORAGE) |
                                              fault_bit(FaultClass::CONFIG));
    CHECK(lowest_fault(both) == FaultClass::CONFIG);

    const uint8_t all = static_cast<uint8_t>(
        fault_bit(FaultClass::SENSE_BUS) | fault_bit(FaultClass::CONFIG) |
        fault_bit(FaultClass::LED_STRING) | fault_bit(FaultClass::STORAGE) |
        fault_bit(FaultClass::INTERNAL));
    CHECK(lowest_fault(all) == FaultClass::SENSE_BUS);

    CHECK(lowest_fault(0) == FaultClass::NONE);
}

TEST(fault_blinks_are_countable_and_correct)
{
    // The whole diagnostic value of FR-IND-4 is that the count read off the
    // board equals the class. One cycle of a 3-fault must show exactly 3.
    for (uint8_t n = kFaultClassMin; n <= kFaultClassMax; ++n)
    {
        IndicatorInput in = running();
        in.faults = fault_bit(static_cast<FaultClass>(n));
        const uint32_t cycle = n * (kFaultOnMs + kFaultOffMs) + kFaultPauseMs;
        CHECK_EQ(count_blinks(in, 0, cycle), static_cast<int>(n));
        // ...and again over three cycles, which catches a pattern that is
        // right once and then drifts out of phase with its own period.
        CHECK_EQ(count_blinks(in, 0, cycle * 3), static_cast<int>(n) * 3);
    }
}

TEST(fault_is_red_and_the_pause_is_dark)
{
    IndicatorInput in = running();
    in.faults = fault_bit(FaultClass::SENSE_BUS);
    in.state_ms = 50; // inside the first blink
    IndicatorRgb c = indicator_render(in);
    CHECK(c.r > 0);
    CHECK_EQ(c.g, 0);
    CHECK_EQ(c.b, 0);

    in.state_ms = 2 * (kFaultOnMs + kFaultOffMs) + 600; // inside the pause
    CHECK(indicator_render(in) == kOff);
}

TEST(a_fault_replaces_the_running_state_rather_than_tinting_it)
{
    // Red must not be mixed with green: "not doing what you asked" has to be
    // legible on its own, and a blend of the two is neither.
    IndicatorInput in = running();
    in.faults = fault_bit(FaultClass::CONFIG);
    in.state_ms = 50;
    CHECK_EQ(indicator_render(in).g, 0);
}

// --- overlays and precedence -------------------------------------------------

TEST(press_blip_is_identical_whatever_the_press_will_do)
{
    // FR-IND-7: the acknowledgement answers "did that register", so it cannot
    // vary with the outcome. Same 80 ms white from every state, including a
    // faulted one and a staged one.
    const IndicatorState states[] = {
        IndicatorState::BOOTING, IndicatorState::RUNNING,
        IndicatorState::RUNNING_OFFLINE, IndicatorState::SOFTAP,
        IndicatorState::STAGED, IndicatorState::APPLYING};

    for (IndicatorState s : states)
    {
        IndicatorInput in = running();
        in.state = s;
        in.staged_window_ms = 30000;
        in.staged_remaining_ms = 15000;
        in.faults = fault_bit(FaultClass::INTERNAL);
        in.blip_ms = 0;
        CHECK(indicator_render(in) == kFullWhite);
        in.blip_ms = kBlipMs - 1;
        CHECK(indicator_render(in) == kFullWhite);
    }
}

TEST(press_blip_ends_and_the_state_returns)
{
    IndicatorInput in = running();
    in.blip_ms = kBlipMs;
    CHECK(indicator_render(in) != kFullWhite);
    in.blip_ms = kNoOverlay;
    CHECK(indicator_render(in) != kFullWhite);
}

TEST(a_press_too_short_to_count_shows_nothing)
{
    // The absence is the answer (FR-IND-7). A press below the debounce
    // threshold never sets blip_ms, so the state is undisturbed.
    IndicatorInput in = running();
    const IndicatorRgb quiet = indicator_render(in);
    in.blip_ms = kNoOverlay;
    CHECK(indicator_render(in) == quiet);
}

TEST(hold_ramp_outranks_everything_and_is_red)
{
    IndicatorInput in = running();
    in.state = IndicatorState::STAGED;
    in.staged_window_ms = 30000;
    in.staged_remaining_ms = 5000;
    in.blip_ms = 0; // a blip that would otherwise win
    in.hold_ms = kHoldRampStartMs;
    const IndicatorRgb c = indicator_render(in);
    CHECK(c.g == 0 && c.b == 0);
}

TEST(hold_below_one_second_does_not_ramp)
{
    IndicatorInput in = running();
    in.hold_ms = kHoldRampStartMs - 1;
    CHECK(indicator_render(in) == indicator_render(running()));
}

TEST(hold_ramp_accelerates_toward_the_erase)
{
    // FR-IND-8: faster each second. Compare blinks in the first second of the
    // ramp against the last, which is the property a person actually reads.
    IndicatorInput in = running();
    in.hold_erase_ms = 5000;

    auto blinks_between = [&](uint32_t a, uint32_t b) {
        int edges = 0;
        bool prev = false;
        for (uint32_t t = a; t < b; t += 5)
        {
            in.hold_ms = t;
            const bool lit = indicator_render(in) != kOff;
            if (lit && !prev)
                ++edges;
            prev = lit;
        }
        return edges;
    };

    const int early = blinks_between(1000, 2000);
    const int late = blinks_between(4000, 5000);
    CHECK(early >= 1);
    CHECK(late > early);
}

TEST(releasing_early_returns_to_the_previous_state)
{
    // FR-IND-8's last clause. Release is hold_ms = 0, and nothing about the
    // ramp may persist into the state that follows.
    IndicatorInput held = running();
    held.hold_ms = 4900;
    CHECK(indicator_render(held) != indicator_render(running()));
    held.hold_ms = 0;
    CHECK(indicator_render(held) == indicator_render(running()));
}

TEST(profiler_flash_is_brief_and_white)
{
    IndicatorInput in = running();
    in.flash_ms = 0;
    CHECK(indicator_render(in) == kFullWhite);
    in.flash_ms = kFlashMs;
    CHECK(indicator_render(in) != kFullWhite);
}

TEST(staged_outranks_a_fault)
{
    // A deadline you have seconds to act on beats a diagnosis that will still
    // be readable afterwards.
    IndicatorInput in = running();
    in.state = IndicatorState::STAGED;
    in.staged_window_ms = 30000;
    in.staged_remaining_ms = 30000;
    in.faults = fault_bit(FaultClass::SENSE_BUS);
    const IndicatorRgb c = indicator_render(in);
    CHECK(c.g > 0); // amber, not red
}

TEST(applying_is_solid_amber_and_outranks_everything_but_the_button)
{
    IndicatorInput in = running();
    in.state = IndicatorState::APPLYING;
    in.faults = fault_bit(FaultClass::STORAGE);
    for (uint32_t t = 0; t < 3000; t += 10)
    {
        in.state_ms = t;
        const IndicatorRgb c = indicator_render(in);
        CHECK(c.r > 0 && c.g > 0 && c.b == 0);
    }
}

// --- the accelerating countdown ---------------------------------------------

TEST(accel_phase_is_monotonic_and_starts_at_zero)
{
    CHECK(std::fabs(accel_phase(0, 30000, 800, 120)) < 1e-6f);
    float prev = -1.0f;
    for (uint32_t t = 0; t <= 30000; t += 50)
    {
        const float p = accel_phase(t, 30000, 800, 120);
        CHECK(p >= prev);
        prev = p;
    }
}

TEST(accel_phase_matches_the_period_at_each_end)
{
    // One cycle near the start should take about p_start, and one near the
    // end about p_end. This is the claim the whole integral exists to make.
    const uint32_t W = 30000, p0 = 800, p1 = 120;
    const float near_start = accel_phase(p0, W, p0, p1) - accel_phase(0, W, p0, p1);
    CHECK(near_start > 0.9f);
    CHECK(near_start < 1.1f);

    const float near_end = accel_phase(W, W, p0, p1) - accel_phase(W - p1, W, p0, p1);
    CHECK(near_end > 0.9f);
    CHECK(near_end < 1.1f);
}

TEST(accel_phase_degenerates_cleanly_when_the_period_is_constant)
{
    CHECK(std::fabs(accel_phase(1000, 5000, 200, 200) - 5.0f) < 1e-3f);
}

TEST(accel_phase_survives_a_countdown_that_ran_off_its_end)
{
    // Nothing should render past the window, but a NaN here would be a dark
    // pixel with no explanation, so it must degrade rather than blow up.
    const float p = accel_phase(45000, 30000, 800, 120);
    CHECK(std::isfinite(p));
    CHECK(p > accel_phase(30000, 30000, 800, 120));
    CHECK(std::isfinite(accel_phase(100, 0, 800, 120)));
    CHECK(std::isfinite(accel_phase(100, 30000, 0, 120)));
}

TEST(staged_blink_speeds_up_as_the_window_expires)
{
    // FR-IND-3, and the reason the window length is an input: the rate is a
    // function of how much is left, not of how long the device has been up.
    IndicatorInput in{};
    in.state = IndicatorState::STAGED;
    in.brightness = 255;
    in.staged_window_ms = 30000;

    auto blinks_in_last = [&](uint32_t from_remaining, uint32_t to_remaining) {
        int edges = 0;
        bool prev = false;
        for (uint32_t r = from_remaining; r > to_remaining; r -= 5)
        {
            in.staged_remaining_ms = r;
            const bool lit = indicator_render(in) != kOff;
            if (lit && !prev)
                ++edges;
            prev = lit;
        }
        return edges;
    };

    const int first_5s = blinks_in_last(30000, 25000);
    const int last_5s = blinks_in_last(5000, 0);
    CHECK(first_5s >= 1);
    CHECK(last_5s > first_5s * 2);
}

TEST(staged_elapsed_clamps_a_remaining_larger_than_its_window)
{
    // Tested here rather than through indicator_render, because the clamp is
    // invisible from the rendered colour: unclamped, the subtraction wraps to
    // ~4.3e9 ms, and at that magnitude a float's mantissa quantises the phase
    // to whole cycles, so the pixel happens to come out the same amber. That
    // agreement is an accident of precision. Asserting the arithmetic is the
    // only way to state the intent.
    CHECK_EQ(staged_elapsed_ms(30000, 30000), 0u);
    CHECK_EQ(staged_elapsed_ms(30000, 20000), 10000u);
    CHECK_EQ(staged_elapsed_ms(30000, 0), 30000u);
    CHECK_EQ(staged_elapsed_ms(30000, 45000), 0u);   // would wrap
    CHECK_EQ(staged_elapsed_ms(0, 1000), 0u);
    // Never past the window it belongs to, whatever it is fed.
    for (uint32_t r = 0; r <= 60000; r += 250)
        CHECK(staged_elapsed_ms(30000, r) <= 30000u);
}

TEST(staged_keeps_blinking_across_the_clamped_boundary)
{
    // The contract the clamp exists to protect, stated where a person would
    // look for it: the pixel is still blinking as the window opens.
    IndicatorInput in{};
    in.state = IndicatorState::STAGED;
    in.brightness = 255;
    in.staged_window_ms = 30000;

    bool ever_lit = false, ever_dark = false;
    for (uint32_t r = 30500; r > 28500; r -= 5)
    {
        in.staged_remaining_ms = r;
        const IndicatorRgb c = indicator_render(in);
        CHECK(c.b == 0); // amber or dark, never a wrapped-phase artefact
        if (c == kOff)
            ever_dark = true;
        else
            ever_lit = true;
    }
    CHECK(ever_lit);
    CHECK(ever_dark);
}

// --- base states -------------------------------------------------------------

TEST(healthy_running_is_still)
{
    // FR-IND-3: solid means steady, blinking means the device wants
    // something. A healthy machine must never move.
    IndicatorInput in = running();
    const IndicatorRgb first = indicator_render(in);
    CHECK(first.g > 0);
    CHECK(first.r == 0 && first.b == 0);
    for (uint32_t t = 0; t < 10000; t += 37)
    {
        in.state_ms = t;
        CHECK(indicator_render(in) == first);
    }
}

TEST(running_is_dimmer_than_a_state_that_wants_attention)
{
    IndicatorInput run = running();
    IndicatorInput ap = running();
    ap.state = IndicatorState::SOFTAP;
    ap.state_ms = 0; // lit half of the blink
    CHECK(indicator_render(run).g < indicator_render(ap).b);
}

TEST(softap_blinks_and_is_blue)
{
    IndicatorInput in = running();
    in.state = IndicatorState::SOFTAP;
    in.state_ms = 0;
    const IndicatorRgb lit = indicator_render(in);
    CHECK(lit.b > 0);
    CHECK(lit.r == 0 && lit.g == 0);
    // Blue rather than green: a mode awaiting action, kept off the red/green
    // axis entirely (FR-IND-2).
    CHECK_EQ(count_blinks(in, 0, kSoftApPeriodMs * 4), 4);
}

TEST(offline_breathes_rather_than_blinking)
{
    // Distinguishable from SoftAP by pattern alone, which is the point: both
    // are "not reachable", and only one of them wants something from you.
    IndicatorInput in = running();
    in.state = IndicatorState::RUNNING_OFFLINE;

    uint8_t lo = 255, hi = 0;
    bool ever_dark = false;
    for (uint32_t t = 0; t < kBreathePeriodMs; t += 10)
    {
        in.state_ms = t;
        const IndicatorRgb c = indicator_render(in);
        if (c.g < lo)
            lo = c.g;
        if (c.g > hi)
            hi = c.g;
        if (c == kOff)
            ever_dark = true;
    }
    CHECK(hi > lo);        // it moves
    CHECK(!ever_dark);     // ...but never goes out, or it is a blink
    CHECK_EQ(count_blinks(in, 0, kBreathePeriodMs * 2), 1);
}

TEST(booting_is_solid_white)
{
    IndicatorInput in{};
    in.brightness = 255;
    in.state = IndicatorState::BOOTING;
    for (uint32_t t = 0; t < 3000; t += 50)
    {
        in.state_ms = t;
        CHECK(indicator_render(in) == kFullWhite);
    }
}

// --- brightness (FR-IND-5) ---------------------------------------------------

TEST(brightness_zero_is_genuinely_dark_in_every_state)
{
    // Including the ones that shout. A backbox the owner asked to be dark
    // stays dark even while a fault is active — the API still reports it.
    const IndicatorState states[] = {
        IndicatorState::BOOTING, IndicatorState::RUNNING,
        IndicatorState::RUNNING_OFFLINE, IndicatorState::SOFTAP,
        IndicatorState::STAGED, IndicatorState::APPLYING};
    for (IndicatorState s : states)
    {
        IndicatorInput in{};
        in.brightness = 0;
        in.state = s;
        in.faults = fault_bit(FaultClass::SENSE_BUS);
        in.staged_window_ms = 30000;
        in.staged_remaining_ms = 1000;
        in.blip_ms = 0;
        in.hold_ms = 4000;
        for (uint32_t t = 0; t < 2000; t += 100)
        {
            in.state_ms = t;
            CHECK(indicator_render(in) == kOff);
        }
    }
}

TEST(brightness_scales_without_changing_the_pattern)
{
    IndicatorInput bright = running();
    IndicatorInput dimmed = running();
    dimmed.brightness = 32;
    CHECK(indicator_render(dimmed).g < indicator_render(bright).g);

    // A fault at low brightness must still blink the same count — dimming is
    // not allowed to make a diagnosis unreadable by rounding it to nothing.
    IndicatorInput f = dimmed;
    f.faults = fault_bit(FaultClass::LED_STRING);
    const uint32_t cycle = 4 * (kFaultOnMs + kFaultOffMs) + kFaultPauseMs;
    CHECK_EQ(count_blinks(f, 0, cycle), 4);
}

// --- naming ------------------------------------------------------------------

TEST(every_state_and_class_has_a_name)
{
    const IndicatorState states[] = {
        IndicatorState::BOOTING, IndicatorState::RUNNING,
        IndicatorState::RUNNING_OFFLINE, IndicatorState::SOFTAP,
        IndicatorState::STAGED, IndicatorState::APPLYING};
    for (IndicatorState s : states)
        CHECK(std::string(indicator_state_name(s)) != "unknown");

    for (uint8_t n = kFaultClassMin; n <= kFaultClassMax; ++n)
        CHECK(std::string(fault_class_name(static_cast<FaultClass>(n))) != "unknown");
    CHECK(std::string(fault_class_name(FaultClass::NONE)) == "none");
}

int main() { return ooe::test::run_all(); }
