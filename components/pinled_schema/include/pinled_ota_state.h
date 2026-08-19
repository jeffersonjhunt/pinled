#pragma once

/**
 * @file pinled_ota_state.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief The OTA staging state machine, as pure functions (FR-OTA-2, FR-OTA-7).
 * @version 0.1.0
 * @date 2026-08-18
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * The same split as `pinled_indicator.h`: everything with a rule in it lives
 * here, IDF-free, so the rules get ordinary host tests; the component that
 * owns the flash and the timer (`components/pinled_ota`) is deliberately the
 * half with nothing worth asserting.
 *
 * @par The word
 * Phase and generation live in ONE 32-bit word, exactly like the profiler's
 * hand-off in main.h and for the same reason: every transition is a
 * compare-exchange on the whole thing, so the phase can never change without
 * the generation changing in the same instant. That is what makes a second
 * upload during a pending window a REFUSAL rather than a silent replacement
 * (FR-OTA-2) — the second `begin` sees a word it cannot advance and gives up,
 * instead of restarting a staging someone else is about to confirm.
 *
 * @par Transitions return a word, not a bool
 * Each transition function is pure: it takes the word it read and returns the
 * word to compare-exchange in, or the SAME word to mean "refused from this
 * phase". Refusal and idempotence never collide because every legal
 * transition changes at least the phase bits. The caller loops on the CAS as
 * usual; these functions only decide what is legal.
 *
 * @par Expiry is a decision made twice
 * The deadline is checked by the expiry timer (which retires the word) AND at
 * the moment of confirmation (which refuses a press that raced it). Both use
 * the same `ota_expired()` here, and both are wrap-safe: milliseconds since
 * boot wrap at 49 days, and a staged image must not become unconfirmable —
 * or worse, eternally confirmable — because the upload straddled the wrap.
 */

#include <cstdint>

namespace ooe::pinled
{
    /// Where a staging attempt is. Values are packed into the low bits of the
    /// word; the names are what `ota_phase_name()` reports and the API shows.
    enum class OtaPhase : uint8_t
    {
        IDLE = 0,      ///< no upload in progress, nothing staged
        RECEIVING = 1, ///< an upload is streaming into the inactive slot
        PENDING = 2,   ///< image verified and staged; the window is open
        APPLYING = 3,  ///< confirmed; boot partition switched, restart imminent
    };

    inline constexpr uint32_t kOtaPhaseBits = 2;
    inline constexpr uint32_t kOtaPhaseMask = (1u << kOtaPhaseBits) - 1;

    constexpr uint32_t ota_pack(OtaPhase p, uint32_t gen)
    {
        return (gen << kOtaPhaseBits) | static_cast<uint32_t>(p);
    }
    constexpr OtaPhase ota_phase(uint32_t w)
    {
        return static_cast<OtaPhase>(w & kOtaPhaseMask);
    }
    constexpr uint32_t ota_gen(uint32_t w) { return w >> kOtaPhaseBits; }

    const char *ota_phase_name(OtaPhase p);

    /// A new upload wants to start. Legal only from IDLE — an upload during
    /// another upload, a pending window, or the restart gap is refused
    /// (FR-OTA-2). Advances the generation, so an expiry timer armed for the
    /// previous staging can never retire this one.
    uint32_t ota_on_begin(uint32_t w);

    /// The upload completed and the image verified. Legal only from
    /// RECEIVING, and keeps the generation: the staged image IS the upload
    /// that just finished, not a new attempt.
    uint32_t ota_on_staged(uint32_t w);

    /// The staging is over without an apply: the upload failed, the window
    /// expired, or the user discarded it. Legal from RECEIVING and PENDING.
    /// Keeps the generation — abandoning is the end of this attempt, not the
    /// start of another — so a stale expiry firing after a discard changes
    /// nothing.
    uint32_t ota_on_abandon(uint32_t w);

    /// The button was pressed. Legal only from PENDING and only before the
    /// deadline: a press that races the expiry loses, because the person may
    /// be looking at an indicator that already stopped blinking.
    uint32_t ota_on_confirm(uint32_t w, uint32_t now_ms, uint32_t deadline_ms);

    /// Wrap-safe "is the deadline behind us". Milliseconds since boot wrap at
    /// 49 days; the signed difference keeps a window that straddles the wrap
    /// exactly as long as any other.
    bool ota_expired(uint32_t now_ms, uint32_t deadline_ms);

    /// Milliseconds left in the window, zero once expired. What the API
    /// reports and the SPA counts down (FR-OTA-2).
    uint32_t ota_remaining_ms(uint32_t now_ms, uint32_t deadline_ms);
} // namespace ooe::pinled
