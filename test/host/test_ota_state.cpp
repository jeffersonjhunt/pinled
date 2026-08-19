/**
 * @file test_ota_state.cpp
 * @brief The OTA staging state machine: every transition, and both races.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * The machine's whole job is to make two races boring (FR-OTA-2): a second
 * upload during a pending window must be a refusal, and a confirmation must
 * lose deterministically to the expiry it raced. Both are decided by pure
 * functions on one word, so both are decided here, on the host, rather than
 * by trying to time two HTTP requests against a 30-second window on a bench.
 */

#include "harness.h"
#include "pinled_ota_state.h"

#include <cstring>

using namespace ooe::pinled;

// ------------------------------------------------------------- the word --

TEST(pack_and_unpack_round_trip)
{
    const uint32_t w = ota_pack(OtaPhase::PENDING, 12345);
    CHECK(ota_phase(w) == OtaPhase::PENDING);
    CHECK_EQ(ota_gen(w), static_cast<uint32_t>(12345));
}

TEST(every_phase_has_a_name)
{
    for (uint8_t p = 0; p <= 3; ++p)
        CHECK(std::strcmp(ota_phase_name(static_cast<OtaPhase>(p)), "unknown") != 0);
}

// ------------------------------------------------------------ lifecycle --

TEST(the_happy_path_walks_idle_to_applying_on_one_generation)
{
    const uint32_t idle = ota_pack(OtaPhase::IDLE, 4);

    const uint32_t receiving = ota_on_begin(idle);
    CHECK(ota_phase(receiving) == OtaPhase::RECEIVING);
    CHECK_EQ(ota_gen(receiving), static_cast<uint32_t>(5));

    const uint32_t pending = ota_on_staged(receiving);
    CHECK(ota_phase(pending) == OtaPhase::PENDING);
    CHECK_EQ(ota_gen(pending), static_cast<uint32_t>(5));

    const uint32_t applying = ota_on_confirm(pending, 1000, 31000);
    CHECK(ota_phase(applying) == OtaPhase::APPLYING);
    // The generation never moves after begin: the image confirmed IS the
    // image uploaded, and every observer holding the old word can tell.
    CHECK_EQ(ota_gen(applying), static_cast<uint32_t>(5));
}

TEST(begin_is_refused_from_every_phase_but_idle)
{
    // FR-OTA-2's refusal, phase by phase. The convention is "same word back
    // means no": a legal begin always changes phase AND generation, so
    // equality is unambiguous.
    for (OtaPhase p : {OtaPhase::RECEIVING, OtaPhase::PENDING, OtaPhase::APPLYING})
    {
        const uint32_t w = ota_pack(p, 7);
        CHECK_EQ(ota_on_begin(w), w);
    }
}

TEST(staged_is_refused_except_from_receiving)
{
    for (OtaPhase p : {OtaPhase::IDLE, OtaPhase::PENDING, OtaPhase::APPLYING})
    {
        const uint32_t w = ota_pack(p, 7);
        CHECK_EQ(ota_on_staged(w), w);
    }
}

TEST(abandon_ends_receiving_and_pending_but_not_applying)
{
    // Once APPLYING, the boot partition is switched and the restart is
    // scheduled; nothing may quietly walk that back to IDLE and let a new
    // upload race the reboot.
    CHECK(ota_phase(ota_on_abandon(ota_pack(OtaPhase::RECEIVING, 3))) == OtaPhase::IDLE);
    CHECK(ota_phase(ota_on_abandon(ota_pack(OtaPhase::PENDING, 3))) == OtaPhase::IDLE);

    const uint32_t applying = ota_pack(OtaPhase::APPLYING, 3);
    CHECK_EQ(ota_on_abandon(applying), applying);
    const uint32_t idle = ota_pack(OtaPhase::IDLE, 3);
    CHECK_EQ(ota_on_abandon(idle), idle);
}

TEST(abandon_keeps_the_generation)
{
    // A stale expiry timer holds the OLD word for its compare-exchange. If
    // abandoning bumped the generation, the timer's stale word would still
    // mismatch — but a FRESH begin after the abandon would then be two
    // generations on, and an observer diffing generations would count an
    // upload that never happened.
    const uint32_t pending = ota_pack(OtaPhase::PENDING, 9);
    CHECK_EQ(ota_gen(ota_on_abandon(pending)), static_cast<uint32_t>(9));
}

// --------------------------------------------------------------- expiry --

TEST(confirm_wins_before_the_deadline_and_loses_from_the_deadline_on)
{
    const uint32_t pending = ota_pack(OtaPhase::PENDING, 2);

    CHECK(ota_phase(ota_on_confirm(pending, 29999, 30000)) == OtaPhase::APPLYING);
    // AT the deadline is already too late: the timer fires at this instant,
    // and the press racing it must lose by rule, not by scheduling.
    CHECK_EQ(ota_on_confirm(pending, 30000, 30000), pending);
    CHECK_EQ(ota_on_confirm(pending, 30001, 30000), pending);
}

TEST(confirm_is_refused_from_every_phase_but_pending)
{
    for (OtaPhase p : {OtaPhase::IDLE, OtaPhase::RECEIVING, OtaPhase::APPLYING})
    {
        const uint32_t w = ota_pack(p, 2);
        CHECK_EQ(ota_on_confirm(w, 0, 30000), w);
    }
}

TEST(a_window_straddling_the_49_day_wrap_is_a_window_like_any_other)
{
    // Staged 200 ms before the millisecond counter wraps, 30 s window: the
    // deadline is numerically TINY while `now` is numerically enormous.
    const uint32_t deadline = 0xFFFFFF38u + 30000u; // wraps to 0x74B8...
    uint32_t now = 0xFFFFFF38u;

    CHECK(!ota_expired(now, deadline));
    CHECK_EQ(ota_remaining_ms(now, deadline), static_cast<uint32_t>(30000));

    now += 29999; // one ms short, on the far side of the wrap
    CHECK(!ota_expired(now, deadline));
    CHECK_EQ(ota_remaining_ms(now, deadline), static_cast<uint32_t>(1));

    now += 1;
    CHECK(ota_expired(now, deadline));
    CHECK_EQ(ota_remaining_ms(now, deadline), static_cast<uint32_t>(0));
}

TEST(an_expired_window_does_not_come_back_half_a_wrap_later)
{
    // The failure mode the signed difference exists to prevent: 24.8 days
    // after an expiry, naive unsigned comparison would see the deadline
    // "ahead" again and a forgotten staged image would become confirmable.
    const uint32_t deadline = 30000;
    const uint32_t half_wrap_later = deadline + 0x7FFFFFFFu;

    CHECK(ota_expired(deadline + 1, deadline));
    // Right up to the edge of representability it stays expired...
    CHECK(ota_expired(half_wrap_later, deadline));
    // ...and the pure confirm refuses it.
    const uint32_t pending = ota_pack(OtaPhase::PENDING, 1);
    CHECK_EQ(ota_on_confirm(pending, half_wrap_later, deadline), pending);
}

// ---------------------------------------------------------------- races --

TEST(a_second_upload_during_a_pending_window_cannot_advance_the_word)
{
    // The CAS choreography in miniature. Uploader B reads the word while A's
    // image is pending; whatever B computes, it is the SAME word, so B's
    // compare-exchange is a no-op and A's staging survives untouched.
    const uint32_t pending = ota_pack(OtaPhase::PENDING, 6);
    CHECK_EQ(ota_on_begin(pending), pending);
}

TEST(a_stale_expiry_cannot_retire_the_next_staging)
{
    // The expiry timer for generation N captures the PENDING word it means
    // to retire. If the user discards N and uploads again before the timer
    // fires, the word is PENDING at generation N+1 — and the timer's stale
    // word no longer matches anything, so its compare-exchange fails.
    const uint32_t staged_n = ota_pack(OtaPhase::PENDING, 6);
    const uint32_t discarded = ota_on_abandon(staged_n);
    const uint32_t staged_n1 = ota_on_staged(ota_on_begin(discarded));

    CHECK(ota_phase(staged_n1) == OtaPhase::PENDING);
    CHECK(staged_n1 != staged_n); // the stale CAS has nothing to hit
    CHECK_EQ(ota_gen(staged_n1), static_cast<uint32_t>(7));
}

int main() { return ooe::test::run_all(); }
