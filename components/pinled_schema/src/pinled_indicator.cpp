/**
 * @file pinled_indicator.cpp
 * @brief Status-pixel pattern generation (FR-IND-1..8).
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "pinled_indicator.h"

#include <cmath>

namespace ooe::pinled
{
    namespace
    {
        constexpr IndicatorRgb kBlack{0, 0, 0};
        constexpr IndicatorRgb kWhite{255, 255, 255};
        constexpr IndicatorRgb kGreen{0, 255, 0};
        constexpr IndicatorRgb kBlue{0, 0, 255};
        constexpr IndicatorRgb kAmber{255, 140, 0};
        constexpr IndicatorRgb kRed{255, 0, 0};

        constexpr uint8_t scale8(uint8_t v, uint8_t s)
        {
            return static_cast<uint8_t>((static_cast<uint16_t>(v) * s) / 255u);
        }

        constexpr IndicatorRgb dim(const IndicatorRgb &c, uint8_t s)
        {
            return IndicatorRgb{scale8(c.r, s), scale8(c.g, s), scale8(c.b, s)};
        }

        /// Symmetric square wave: lit for the first half of every period.
        bool square(uint32_t elapsed_ms, uint32_t period_ms)
        {
            if (period_ms == 0)
                return true;
            return (elapsed_ms % period_ms) < (period_ms / 2);
        }

        /// FR-IND-4: `n` blinks of 200/200, then a 1.2 s pause, repeating.
        bool fault_blink(uint32_t elapsed_ms, uint8_t n)
        {
            const uint32_t blink = kFaultOnMs + kFaultOffMs;
            const uint32_t train = blink * n;
            const uint32_t cycle = train + kFaultPauseMs;
            const uint32_t t = elapsed_ms % cycle;
            if (t >= train)
                return false; // the pause, which is what makes it countable
            return (t % blink) < kFaultOnMs;
        }

        /// Triangle 0..255 over one period. Triangle rather than a sine
        /// because the eye is being asked "is this moving", not "is this
        /// smooth", and it costs no libm call.
        uint8_t triangle(uint32_t elapsed_ms, uint32_t period_ms)
        {
            if (period_ms == 0)
                return 255;
            const uint32_t t = elapsed_ms % period_ms;
            const uint32_t half = period_ms / 2;
            if (half == 0)
                return 255;
            const uint32_t up = (t < half) ? t : (period_ms - t);
            const uint32_t v = (up * 255u) / half;
            return static_cast<uint8_t>(v > 255u ? 255u : v);
        }
    } // namespace

    FaultClass lowest_fault(uint8_t faults)
    {
        for (uint8_t c = kFaultClassMin; c <= kFaultClassMax; ++c)
            if (faults & static_cast<uint8_t>(1u << c))
                return static_cast<FaultClass>(c);
        return FaultClass::NONE;
    }

    const char *fault_class_name(FaultClass c)
    {
        switch (c)
        {
        case FaultClass::NONE:
            return "none";
        case FaultClass::SENSE_BUS:
            return "sense bus";
        case FaultClass::CONFIG:
            return "configuration";
        case FaultClass::LED_STRING:
            return "LED string";
        case FaultClass::STORAGE:
            return "storage";
        case FaultClass::INTERNAL:
            return "internal";
        }
        return "unknown";
    }

    const char *indicator_state_name(IndicatorState s)
    {
        switch (s)
        {
        case IndicatorState::BOOTING:
            return "booting";
        case IndicatorState::RUNNING:
            return "running";
        case IndicatorState::RUNNING_OFFLINE:
            return "running, no network";
        case IndicatorState::SOFTAP:
            return "SoftAP";
        case IndicatorState::STAGED:
            return "image staged";
        case IndicatorState::APPLYING:
            return "applying firmware";
        }
        return "unknown";
    }

    float accel_phase(uint32_t elapsed_ms, uint32_t window_ms,
                      uint32_t p_start_ms, uint32_t p_end_ms)
    {
        if (p_start_ms == 0)
            return 0.0f;
        // Past the window the rate stops accelerating and simply holds at the
        // fast end. Nothing should still be rendering here, but a countdown
        // that ran off its own end must not produce a NaN.
        if (window_ms == 0 || elapsed_ms >= window_ms)
        {
            const float at_end = (p_start_ms == p_end_ms)
                                     ? 0.0f
                                     : (static_cast<float>(window_ms) /
                                        (static_cast<float>(p_end_ms) - static_cast<float>(p_start_ms))) *
                                           std::log(static_cast<float>(p_end_ms) /
                                                    static_cast<float>(p_start_ms));
            const float over = (p_end_ms == 0) ? 0.0f
                                               : static_cast<float>(elapsed_ms - window_ms) /
                                                     static_cast<float>(p_end_ms);
            return at_end + over;
        }
        const float t = static_cast<float>(elapsed_ms);
        const float W = static_cast<float>(window_ms);
        const float p0 = static_cast<float>(p_start_ms);
        const float k = static_cast<float>(p_end_ms) - p0;
        if (k == 0.0f)
            return t / p0; // constant period: the integral degenerates
        const float p = p0 + k * t / W;
        if (p <= 0.0f)
            return 0.0f;
        return (W / k) * std::log(p / p0);
    }

    bool accel_on(uint32_t elapsed_ms, uint32_t window_ms,
                  uint32_t p_start_ms, uint32_t p_end_ms)
    {
        const float phase = accel_phase(elapsed_ms, window_ms, p_start_ms, p_end_ms);
        return (phase - std::floor(phase)) < 0.5f;
    }

    uint32_t staged_elapsed_ms(uint32_t window_ms, uint32_t remaining_ms)
    {
        return window_ms - (remaining_ms > window_ms ? window_ms : remaining_ms);
    }

    IndicatorRgb indicator_render(const IndicatorInput &in)
    {
        // FR-IND-5. An early-out, not the mechanism: the final dim() scales
        // every path by `brightness` and 0 there is already exactly black.
        // Kept because "off means off" should be readable at the top of the
        // function rather than inferred from arithmetic at the bottom of it.
        if (in.brightness == 0)
            return kBlack;

        IndicatorRgb c{};

        if (in.hold_ms >= kHoldRampStartMs)
        {
            // FR-IND-8. The ramp runs from the one-second threshold to the
            // erase, so what accelerates is progress toward the destructive
            // act rather than raw time held.
            const uint32_t erase = (in.hold_erase_ms > kHoldRampStartMs)
                                       ? in.hold_erase_ms
                                       : kHoldRampStartMs + 1;
            c = accel_on(in.hold_ms - kHoldRampStartMs, erase - kHoldRampStartMs,
                         kHoldSlowMs, kHoldFastMs)
                    ? kRed
                    : kBlack;
        }
        else if (in.blip_ms < kBlipMs)
        {
            // FR-IND-7. Above the state, above a fault, and identical every
            // time: the question it answers is "did that register", and an
            // acknowledgement that varied with what the press did would not
            // answer it.
            c = kWhite;
        }
        else if (in.state == IndicatorState::APPLYING)
        {
            c = kAmber;
        }
        else if (in.state == IndicatorState::STAGED)
        {
            const uint32_t window = in.staged_window_ms;
            c = accel_on(staged_elapsed_ms(window, in.staged_remaining_ms), window,
                         kStagedSlowMs, kStagedFastMs)
                    ? kAmber
                    : kBlack;
        }
        else if (in.flash_ms < kFlashMs)
        {
            c = kWhite;
        }
        else if (const FaultClass f = lowest_fault(in.faults); f != FaultClass::NONE)
        {
            c = fault_blink(in.state_ms, static_cast<uint8_t>(f)) ? kRed : kBlack;
        }
        else
        {
            switch (in.state)
            {
            case IndicatorState::RUNNING:
                c = dim(kGreen, kRunningDim);
                break;
            case IndicatorState::RUNNING_OFFLINE:
            {
                const uint8_t t = triangle(in.state_ms, kBreathePeriodMs);
                const uint8_t s = static_cast<uint8_t>(
                    kBreatheFloor + ((255u - kBreatheFloor) * t) / 255u);
                c = dim(kGreen, s);
                break;
            }
            case IndicatorState::SOFTAP:
                c = square(in.state_ms, kSoftApPeriodMs) ? kBlue : kBlack;
                break;
            case IndicatorState::BOOTING:
            default:
                c = kWhite;
                break;
            }
        }

        return dim(c, in.brightness);
    }
} // namespace ooe::pinled
