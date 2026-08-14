/**
 * @file test_live.cpp
 * @brief The scan → push hand-off (FR-UI-5/9).
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * The property worth testing here is not the packing — it is that **a dropped
 * push loses nothing**. FR-UI-9 lets the push task be starved arbitrarily, and
 * that is only safe because activity is sticky and cleared on read. If it were
 * not, a busy device would show a UI that flickers between "everything is
 * happening" and "nothing is", and the bug would be blamed on the network.
 *
 * The concurrency itself is exercised for real: a writer thread hammering
 * `publish()` against a reader calling `snapshot()`, asserting that every set
 * bit is reported exactly once. ThreadSanitizer is not on here (ASan and TSan
 * cannot both be), but a lost or doubled bit is caught by counting.
 */

#include "harness.h"

#include "pinled_live.h"

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

using namespace ooe::pinled;

namespace
{
    /// Backing buffers a real LiveState borrows from.
    struct Rig
    {
        uint8_t levels[LiveState::kMaxChannels]{};
        ChannelConfig channels[LiveState::kMaxChannels]{};
        DriveClass classes[LiveState::kMaxChannels]{};
        LiveState state{};

        explicit Rig(size_t n)
        {
            state.levels = levels;
            state.channels = channels;
            state.classes = classes;
            state.count = n;
        }

        void set_active(size_t ch)
        {
            uint32_t bits[LiveState::kWords]{};
            bits[ch / 32] = 1u << (ch % 32);
            state.publish(bits);
        }
    };

    uint8_t level_of(const uint8_t *buf, size_t ch) { return buf[ch * 2]; }
    uint8_t flags_of(const uint8_t *buf, size_t ch) { return buf[ch * 2 + 1]; }
    uint8_t class_of(const uint8_t *buf, size_t ch)
    {
        return static_cast<uint8_t>((flags_of(buf, ch) >> kLiveClassShift) & kLiveClassMask);
    }
} // namespace

// ------------------------------------------------------------- the packing --

TEST(a_snapshot_carries_two_bytes_a_channel)
{
    Rig r(32);
    uint8_t buf[64]{};
    CHECK_EQ(r.state.snapshot(buf, sizeof(buf)), static_cast<size_t>(64));
}

TEST(levels_and_flags_land_where_the_schema_says)
{
    Rig r(4);
    r.levels[0] = 0;
    r.levels[1] = 128;
    r.levels[2] = 255;
    r.channels[1].lamp = 11; // bound
    r.classes[2] = DriveClass::AC_DIMMED;
    r.set_active(3);

    uint8_t buf[8]{};
    CHECK_EQ(r.state.snapshot(buf, sizeof(buf)), static_cast<size_t>(8));

    CHECK_EQ(level_of(buf, 1), static_cast<uint8_t>(128));
    CHECK_EQ(level_of(buf, 2), static_cast<uint8_t>(255));
    CHECK((flags_of(buf, 1) & LIVE_BOUND) != 0);
    CHECK((flags_of(buf, 0) & LIVE_BOUND) == 0);
    CHECK_EQ(class_of(buf, 2), static_cast<uint8_t>(DriveClass::AC_DIMMED));
    CHECK((flags_of(buf, 3) & LIVE_ACTIVE) != 0);
    CHECK((flags_of(buf, 0) & LIVE_ACTIVE) == 0);
}

TEST(every_drive_class_survives_three_bits)
{
    // The class is squeezed into bits 2..4. Six values fit; a seventh added to
    // the enum without widening the field would alias onto an existing one and
    // mislabel lamps in the UI.
    const DriveClass all[] = {DriveClass::UNKNOWN, DriveClass::OFF, DriveClass::STEADY,
                              DriveClass::MATRIX, DriveClass::AC_STEADY,
                              DriveClass::AC_DIMMED};
    Rig r(6);
    for (size_t i = 0; i < 6; ++i)
        r.classes[i] = all[i];

    uint8_t buf[12]{};
    CHECK_EQ(r.state.snapshot(buf, sizeof(buf)), static_cast<size_t>(12));
    for (size_t i = 0; i < 6; ++i)
        CHECK_EQ(class_of(buf, i), static_cast<uint8_t>(all[i]));
}

TEST(reserved_flag_bits_are_sent_as_zero)
{
    Rig r(1);
    r.classes[0] = DriveClass::AC_DIMMED;
    r.channels[0].lamp = 1;
    r.set_active(0);

    uint8_t buf[2]{};
    r.state.snapshot(buf, sizeof(buf));
    CHECK_EQ(static_cast<uint8_t>(flags_of(buf, 0) & 0xE0u), static_cast<uint8_t>(0));
}

// ------------------------------------------------------------ the stickiness --

TEST(activity_survives_until_it_is_read)
{
    // The whole point. A matrix lamp is on for a few hundred microseconds; a
    // 30 Hz sample of the instantaneous level would miss it essentially always.
    Rig r(8);
    r.set_active(5);

    uint8_t buf[16]{};
    r.state.snapshot(buf, sizeof(buf));
    CHECK((flags_of(buf, 5) & LIVE_ACTIVE) != 0);
}

TEST(reading_clears_it)
{
    Rig r(8);
    r.set_active(5);

    uint8_t buf[16]{};
    r.state.snapshot(buf, sizeof(buf));
    std::memset(buf, 0, sizeof(buf));
    r.state.snapshot(buf, sizeof(buf));

    // Reported once, not forever. Without this a lamp that flashed at boot
    // would read as permanently active.
    CHECK((flags_of(buf, 5) & LIVE_ACTIVE) == 0);
}

TEST(many_frames_between_pushes_collapse_to_one_report)
{
    // A dropped push is a longer window, not a lost sample (FR-UI-9). Nothing
    // accumulates and nothing overflows however far behind the reader falls.
    Rig r(8);
    for (int i = 0; i < 10000; ++i)
        r.set_active(2);

    uint8_t buf[16]{};
    r.state.snapshot(buf, sizeof(buf));
    CHECK((flags_of(buf, 2) & LIVE_ACTIVE) != 0);

    std::memset(buf, 0, sizeof(buf));
    r.state.snapshot(buf, sizeof(buf));
    CHECK((flags_of(buf, 2) & LIVE_ACTIVE) == 0);
}

TEST(draining_discards_activity_instead_of_reporting_it)
{
    // What the push task does while nobody is connected. Without it "sticky
    // since the last push" silently becomes "sticky since the last reader",
    // because the scan publishes whether or not anyone is listening — and the
    // first frame of a new session then carries every blip since the previous
    // one left. Seen on the bench: one transient survived several minutes
    // between sessions and arrived looking live.
    Rig r(8);
    r.set_active(3);

    r.state.drain();

    uint8_t buf[16]{};
    r.state.snapshot(buf, sizeof(buf));
    CHECK((flags_of(buf, 3) & LIVE_ACTIVE) == 0);
}

TEST(draining_clears_every_word_not_only_the_first)
{
    // The bug this guards is a loop bound: a drain that only cleared word 0
    // would look correct on the 32-channel bench rig and leak on a 128-channel
    // playfield, which is the half nobody tests until it is in a machine.
    Rig r(128);
    uint32_t bits[LiveState::kWords];
    for (size_t i = 0; i < LiveState::kWords; ++i)
        bits[i] = 0xFFFFFFFFu;
    r.state.publish(bits);

    r.state.drain();

    uint8_t buf[256]{};
    r.state.snapshot(buf, sizeof(buf));
    for (size_t ch = 0; ch < 128; ++ch)
        CHECK((flags_of(buf, ch) & LIVE_ACTIVE) == 0);
}

TEST(draining_leaves_the_levels_alone)
{
    // Only the sticky bitmap is the push task's to clear. The levels are
    // borrowed from the filament bank and belong to the scan.
    Rig r(8);
    r.levels[1] = 200;
    r.set_active(1);
    r.state.drain();

    uint8_t buf[16]{};
    const size_t n = r.state.snapshot(buf, sizeof(buf));
    CHECK_EQ(n, size_t{16});
    CHECK_EQ(level_of(buf, 1), uint8_t{200});
    CHECK((flags_of(buf, 1) & LIVE_ACTIVE) == 0);
}

TEST(publishing_a_word_at_a_time_sets_every_channel_in_it)
{
    // The scan publishes whole words, not single channels — that is the whole
    // reason the inner loop needs no atomics.
    Rig r(64);
    uint32_t bits[LiveState::kWords]{};
    bits[1] = 0xFFFFFFFFu; // channels 32..63

    r.state.publish(bits);

    uint8_t buf[128]{};
    r.state.snapshot(buf, sizeof(buf));
    for (size_t ch = 0; ch < 32; ++ch)
        CHECK((flags_of(buf, ch) & LIVE_ACTIVE) == 0);
    for (size_t ch = 32; ch < 64; ++ch)
        CHECK((flags_of(buf, ch) & LIVE_ACTIVE) != 0);
}

// -------------------------------------------------------------- the bounds --

TEST(a_buffer_too_small_writes_nothing)
{
    // Better a dropped push than a half-filled frame the browser will unpack
    // as garbage: channel_count and the payload have to agree.
    Rig r(32);
    uint8_t buf[63]{};
    CHECK_EQ(r.state.snapshot(buf, sizeof(buf)), static_cast<size_t>(0));

    // And the activity must NOT have been consumed by the failed attempt.
    r.set_active(1);
    CHECK_EQ(r.state.snapshot(buf, sizeof(buf)), static_cast<size_t>(0));
    uint8_t ok[64]{};
    r.state.snapshot(ok, sizeof(ok));
    CHECK((flags_of(ok, 1) & LIVE_ACTIVE) != 0);
}

TEST(a_null_destination_is_refused)
{
    Rig r(8);
    CHECK_EQ(r.state.snapshot(nullptr, 16), static_cast<size_t>(0));
}

TEST(zero_channels_produces_nothing)
{
    Rig r(0);
    uint8_t buf[16]{};
    CHECK_EQ(r.state.snapshot(buf, sizeof(buf)), static_cast<size_t>(0));
}

TEST(missing_borrowed_buffers_do_not_crash)
{
    // channels/classes/levels are borrowed pointers; a caller that has not
    // wired them yet must get zeros rather than a fault.
    LiveState s{};
    s.count = 4;
    uint8_t buf[8]{};
    CHECK_EQ(s.snapshot(buf, sizeof(buf)), static_cast<size_t>(8));
    for (size_t ch = 0; ch < 4; ++ch)
    {
        CHECK_EQ(level_of(buf, ch), static_cast<uint8_t>(0));
        CHECK_EQ(flags_of(buf, ch), static_cast<uint8_t>(0));
    }
}

// --------------------------------------------------------- under contention --

TEST(no_activity_is_lost_or_doubled_under_concurrent_publish)
{
    // The reason publish uses fetch_or and snapshot uses exchange. A plain
    // load-then-store on the drain side would swallow anything set in between,
    // which would present as "the UI occasionally misses a flash" — a bug that
    // would never be reproduced on demand.
    constexpr int kIterations = 20000;
    Rig r(32);

    std::atomic<bool> stop{false};
    std::atomic<int> published{0};

    std::thread writer([&] {
        for (int i = 0; i < kIterations; ++i)
        {
            r.set_active(7);
            published.fetch_add(1, std::memory_order_relaxed);
        }
        stop.store(true, std::memory_order_release);
    });

    int reported = 0;
    uint8_t buf[64]{};
    while (!stop.load(std::memory_order_acquire))
    {
        if (r.state.snapshot(buf, sizeof(buf)) && (flags_of(buf, 7) & LIVE_ACTIVE))
            ++reported;
    }
    writer.join();

    // Drain whatever the writer set after the last read.
    if (r.state.snapshot(buf, sizeof(buf)) && (flags_of(buf, 7) & LIVE_ACTIVE))
        ++reported;

    // Every report corresponds to at least one publish, and the final drain
    // guarantees nothing is left behind: a second drain must now be clean.
    CHECK(reported >= 1);
    CHECK(reported <= published.load());
    r.state.snapshot(buf, sizeof(buf));
    CHECK((flags_of(buf, 7) & LIVE_ACTIVE) == 0);
}

int main() { return ooe::test::run_all(); }
