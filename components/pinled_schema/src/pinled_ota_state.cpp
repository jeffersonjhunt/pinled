/**
 * @file pinled_ota_state.cpp
 * @brief The OTA staging state machine, as pure functions (FR-OTA-2, FR-OTA-7).
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "pinled_ota_state.h"

namespace ooe::pinled
{
    const char *ota_phase_name(OtaPhase p)
    {
        switch (p)
        {
        case OtaPhase::IDLE: return "idle";
        case OtaPhase::RECEIVING: return "receiving";
        case OtaPhase::PENDING: return "pending";
        case OtaPhase::APPLYING: return "applying";
        }
        return "unknown";
    }

    uint32_t ota_on_begin(uint32_t w)
    {
        if (ota_phase(w) != OtaPhase::IDLE)
            return w;
        return ota_pack(OtaPhase::RECEIVING, ota_gen(w) + 1);
    }

    uint32_t ota_on_staged(uint32_t w)
    {
        if (ota_phase(w) != OtaPhase::RECEIVING)
            return w;
        return ota_pack(OtaPhase::PENDING, ota_gen(w));
    }

    uint32_t ota_on_abandon(uint32_t w)
    {
        const OtaPhase p = ota_phase(w);
        if (p != OtaPhase::RECEIVING && p != OtaPhase::PENDING)
            return w;
        return ota_pack(OtaPhase::IDLE, ota_gen(w));
    }

    uint32_t ota_on_confirm(uint32_t w, uint32_t now_ms, uint32_t deadline_ms)
    {
        if (ota_phase(w) != OtaPhase::PENDING)
            return w;
        if (ota_expired(now_ms, deadline_ms))
            return w;
        return ota_pack(OtaPhase::APPLYING, ota_gen(w));
    }

    bool ota_expired(uint32_t now_ms, uint32_t deadline_ms)
    {
        // The signed difference of unsigned instants: positive once the
        // deadline is behind `now`, whichever side of the 49-day wrap either
        // of them landed on. `>= 0` so the deadline instant itself is already
        // too late — the timer fires AT the deadline, and the press it races
        // must lose deterministically rather than by scheduling luck.
        return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
    }

    uint32_t ota_remaining_ms(uint32_t now_ms, uint32_t deadline_ms)
    {
        if (ota_expired(now_ms, deadline_ms))
            return 0;
        return deadline_ms - now_ms;
    }
} // namespace ooe::pinled
