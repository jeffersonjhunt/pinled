#pragma once

/**
 * @file pinled_indicator.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief Status-pixel pattern generation (FR-IND-1..8).
 * @version 0.2.0
 * @date 2026-08-18
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * One RGB pixel, and the whole of what it shows is decided here: a state and
 * a handful of elapsed-millisecond counters in, a colour out. No IDF, no
 * timer, no LED, no clock of its own — which is what lets the fault counting,
 * the two accelerating countdowns and the overlay precedence be asserted on a
 * host by stepping a number, including the cases that are tedious to stage on
 * hardware (two faults active at once, a hold released at 4.9 s).
 *
 * @par Everything is keyed off elapsed time, not a free-running clock
 * Each periodic pattern restarts when its state is entered. That is
 * deliberate rather than incidental: a fault that begins mid-count is
 * miscounted, and the count is the diagnosis (FR-IND-4).
 *
 * @par Where this does not fully satisfy FR-IND-2
 * "Distinguishable by pattern alone" holds across every state a person must
 * respond to differently *while blinking*. It does not hold between the three
 * solid states — booting (white), running (green, dim) and applying firmware
 * (amber) share a rhythm, and amber against green is exactly the pair that
 * deuteranopia flattens. Booting is transient and needs no response, so the
 * live collision is applying-vs-running. What separates them today is
 * brightness and sequence: applying is full brightness, running is dim, and
 * applying is only ever reached seconds after the staged blink that the
 * operator themselves confirmed. That is weaker than the requirement asks
 * for and it is called out here rather than papered over.
 */

#include <cstddef>
#include <cstdint>

namespace ooe::pinled
{
    /// 8-bit-per-channel colour, in the order a human writes it. Byte order
    /// for the wire is the strip driver's problem, not this file's.
    struct IndicatorRgb
    {
        uint8_t r{0};
        uint8_t g{0};
        uint8_t b{0};
    };

    inline bool operator==(const IndicatorRgb &a, const IndicatorRgb &b)
    {
        return a.r == b.r && a.g == b.g && a.b == b.b;
    }
    inline bool operator!=(const IndicatorRgb &a, const IndicatorRgb &b) { return !(a == b); }

    /// The base states from REQUIREMENTS §5c, in the order they are reached.
    /// The profiler flash, the press blip and the hold ramp are NOT states —
    /// they are overlays, because each has to be visible on top of whatever
    /// the device was already showing.
    enum class IndicatorState : uint8_t
    {
        BOOTING = 0,         ///< white, solid
        RUNNING = 1,         ///< green, solid, dim — network up
        RUNNING_OFFLINE = 2, ///< green, slow breathe — works, not reachable
        SOFTAP = 3,          ///< blue, fast blink — a mode awaiting action
        STAGED = 4,          ///< amber, accelerating blink — press to confirm
        APPLYING = 5,        ///< amber, solid — do not remove power
    };

    /// Fault classes (FR-IND-4). The enum value *is* the blink count, which is
    /// the point of the numbering: there is no table to keep in step. Counting
    /// starts at 2 because one blink and a pause reads as a heartbeat.
    enum class FaultClass : uint8_t
    {
        NONE = 0,
        SENSE_BUS = 2,  ///< scan init failed, or no usable sample rate
        CONFIG = 3,     ///< a stored document was rejected; running on defaults
        LED_STRING = 4, ///< strip init failed or the geometry is impossible
        STORAGE = 5,    ///< the filesystem would not mount or would not write
        INTERNAL = 6,   ///< out of memory, or a task would not start
    };

    inline constexpr uint8_t kFaultClassMin = 2;
    inline constexpr uint8_t kFaultClassMax = 6;

    /// Bit for `faults`, or 0 for NONE and for anything out of range. Faults
    /// are a bitmap rather than a value because several can be true at once —
    /// the indicator shows the lowest, the API reports them all.
    inline constexpr uint8_t fault_bit(FaultClass c)
    {
        return (static_cast<uint8_t>(c) < kFaultClassMin ||
                static_cast<uint8_t>(c) > kFaultClassMax)
                   ? uint8_t{0}
                   : static_cast<uint8_t>(1u << static_cast<uint8_t>(c));
    }

    /// Lowest class set in `faults`, or NONE. FR-IND-4: with several active
    /// the lowest number is shown, because the classes are ordered by what a
    /// person can act on first.
    FaultClass lowest_fault(uint8_t faults);

    const char *fault_class_name(FaultClass c);
    const char *indicator_state_name(IndicatorState s);

    /// Sentinel for an overlay that is not running. Not 0: 0 is a legitimate
    /// "this instant", and a press blip that could not be rendered on the
    /// millisecond it was accepted would defeat FR-IND-7.
    inline constexpr uint32_t kNoOverlay = 0xFFFFFFFFu;

    // --- pattern timing ------------------------------------------------------
    inline constexpr uint32_t kBlipMs = 80;        ///< press acknowledgement (FR-IND-7)
    inline constexpr uint32_t kFlashMs = 120;      ///< profiler pass flash
    inline constexpr uint32_t kFaultOnMs = 200;    ///< FR-IND-4: 200 on...
    inline constexpr uint32_t kFaultOffMs = 200;   ///< ...200 off...
    inline constexpr uint32_t kFaultPauseMs = 1200;///< ...then a 1.2 s pause
    inline constexpr uint32_t kSoftApPeriodMs = 400;
    inline constexpr uint32_t kBreathePeriodMs = 2000; ///< ~0.5 Hz
    inline constexpr uint32_t kHoldRampStartMs = 1000; ///< FR-IND-8 threshold
    inline constexpr uint32_t kStagedSlowMs = 800;     ///< staged blink, window open
    inline constexpr uint32_t kStagedFastMs = 120;     ///< staged blink, at expiry
    inline constexpr uint32_t kHoldSlowMs = 500;       ///< hold ramp at 1 s
    inline constexpr uint32_t kHoldFastMs = 100;       ///< hold ramp at the erase

    /// Dimming applied to the healthy running state before the user's global
    /// brightness. FR-IND-5's concern is a backbox, and "fine" is the state it
    /// sits in for years.
    inline constexpr uint8_t kRunningDim = 64;
    /// Floor of the offline breathe, so it reads as breathing rather than as
    /// blinking with a long on-time.
    inline constexpr uint8_t kBreatheFloor = 24;

    /// Everything the pattern generator is allowed to know. Written by
    /// whoever changes state, read by the indicator task; no field is a
    /// pointer and none of it needs a lock.
    struct IndicatorInput
    {
        IndicatorState state{IndicatorState::BOOTING};
        uint32_t state_ms{0}; ///< since `state` was entered

        uint8_t faults{0}; ///< bitmap, see fault_bit()

        /// STAGED only: how much of the confirmation window is left, and how
        /// long it was. Both are needed — the blink rate is a function of the
        /// fraction elapsed, not of the absolute time (FR-IND-3).
        uint32_t staged_remaining_ms{0};
        uint32_t staged_window_ms{0};

        uint32_t blip_ms{kNoOverlay};  ///< since a press was accepted (FR-IND-7)
        uint32_t flash_ms{kNoOverlay}; ///< since a profiler pass began
        uint32_t hold_ms{0};           ///< button held for this long; 0 = not held
        uint32_t hold_erase_ms{5000};  ///< the erase threshold the ramp runs to

        uint8_t brightness{128}; ///< global scale, 0 = off (FR-IND-5)
    };

    /// Phase, in blink cycles, of a blink whose period sweeps linearly from
    /// `p_start` to `p_end` across `window_ms`.
    ///
    /// The integral rather than a band table, because a table steps the rate
    /// and a step reads as a glitch rather than as acceleration. With
    /// p(t) = p0 + (p1-p0)·t/W the phase is (W/(p1-p0))·ln(p(t)/p0), which is
    /// exact and monotonic, so no phase memory has to be carried between
    /// calls and the generator stays a pure function.
    float accel_phase(uint32_t elapsed_ms, uint32_t window_ms,
                      uint32_t p_start_ms, uint32_t p_end_ms);

    /// Lit half of that accelerating blink.
    bool accel_on(uint32_t elapsed_ms, uint32_t window_ms,
                  uint32_t p_start_ms, uint32_t p_end_ms);

    /// How far into the confirmation window we are, from what is left of it.
    ///
    /// Its own function only because the clamp inside it is invisible from
    /// the rendered colour and would otherwise be untestable: `remaining` and
    /// `window` are written separately and nothing orders them, so a
    /// remaining briefly larger than its window is reachable, and the naive
    /// subtraction wraps to ~4.3e9 ms. Today that wrapped value happens to
    /// render the same amber, because at that magnitude a float's mantissa
    /// quantises the phase to whole cycles — which is luck, not design, and
    /// not a property to leave a countdown resting on.
    uint32_t staged_elapsed_ms(uint32_t window_ms, uint32_t remaining_ms);

    /// What the pixel should show right now. Pure.
    ///
    /// Precedence, highest first, and each step is a requirement rather than
    /// a preference:
    ///   brightness 0   FR-IND-5, off means off
    ///   hold ramp      FR-IND-8, a destructive action one second away
    ///   press blip     FR-IND-7, acknowledged before and independently of
    ///                  whatever the press goes on to do
    ///   applying       do not remove power outranks every diagnosis
    ///   staged         the deadline outranks a fault you can read afterwards
    ///   profiler flash a transient acknowledgement must be visible
    ///   fault          FR-IND-4
    ///   base state
    IndicatorRgb indicator_render(const IndicatorInput &in);
} // namespace ooe::pinled
