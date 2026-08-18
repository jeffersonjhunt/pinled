/**
 * @file test_color_order.cpp
 * @brief Packing a colour for a strip's byte order.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * This is six permutations of three bytes, which sounds too small to test until
 * you notice the driver transmits bits 15:8 FIRST, then 23:16, then 7:0. So the
 * byte that goes out first lives in the middle of the word, and every mistake
 * here produces something that is still correct for grey and wrong for every
 * saturated colour — which is precisely how the bug that prompted this file
 * survived: a warm-white default tint looks warm white through a swapped pair.
 *
 * The assertions are therefore written in terms of TRANSMITTED ORDER, decoded
 * back out of the packed word the same way neopixel.c does, rather than in
 * terms of the packed word itself. A test that restated the packing expression
 * would agree with any bug it contained.
 */

#include "harness.h"

#include "pinled_color_order.h"

using namespace ooe::pinled;

namespace
{
    // Exactly what neopixel.c does: NP_RGB2GREEN, then NP_RGB2RED, then
    // NP_RGB2BLUE — bits 15:8, 23:16, 7:0.
    struct Wire
    {
        uint8_t first, second, third;
    };

    Wire on_the_wire(uint32_t packed)
    {
        return {static_cast<uint8_t>((packed >> 8) & 0xFFu),
                static_cast<uint8_t>((packed >> 16) & 0xFFu),
                static_cast<uint8_t>(packed & 0xFFu)};
    }

    /// Distinct values, so any permutation error is visible.
    constexpr uint8_t R = 0x11, G = 0x22, B = 0x33;

    Wire packed_as(ColorOrder o) { return on_the_wire(pack_for_order(o, R, G, B)); }

    void expect(ColorOrder o, uint8_t a, uint8_t b, uint8_t c)
    {
        const Wire w = packed_as(o);
        CHECK_EQ(w.first, a);
        CHECK_EQ(w.second, b);
        CHECK_EQ(w.third, c);
    }
} // namespace

TEST(grb_is_the_default_and_matches_the_drivers_native_order)
{
    // The behaviour every install had before this field existed. If this case
    // is wrong, adding the field changed the colour of every device in the
    // field, which is the one thing it must not do.
    expect(ColorOrder::UNSPECIFIED, G, R, B);
    expect(ColorOrder::GRB, G, R, B);
}

TEST(rgb_swaps_red_and_green)
{
    // The bench strip, found 2026-08-11: a pure blue came out pink.
    expect(ColorOrder::RGB, R, G, B);
}

TEST(every_order_transmits_what_it_is_named)
{
    expect(ColorOrder::BGR, B, G, R);
    expect(ColorOrder::BRG, B, R, G);
    expect(ColorOrder::RBG, R, B, G);
    expect(ColorOrder::GBR, G, B, R);
}

TEST(every_order_is_a_permutation_and_loses_nothing)
{
    // Catches a duplicated slot — e.g. a copy-paste that writes green twice —
    // which the per-order cases above would only catch if that exact pair
    // happened to be asserted.
    const ColorOrder all[] = {ColorOrder::UNSPECIFIED, ColorOrder::GRB, ColorOrder::RGB,
                              ColorOrder::BGR, ColorOrder::BRG, ColorOrder::RBG,
                              ColorOrder::GBR};
    for (ColorOrder o : all)
    {
        const Wire w = packed_as(o);
        const int mask = (1 << 0) * (w.first == R || w.second == R || w.third == R) +
                         (1 << 1) * (w.first == G || w.second == G || w.third == G) +
                         (1 << 2) * (w.first == B || w.second == B || w.third == B);
        CHECK_EQ(mask, 7);
    }
}

TEST(grey_looks_identical_in_every_order)
{
    // Stated as a test because it is the reason the bug hid. Nothing about a
    // near-white tint can reveal a swapped pair, so no amount of looking at
    // the default colour would ever have found this.
    for (uint8_t v : {uint8_t{0}, uint8_t{128}, uint8_t{255}})
    {
        const uint32_t grb = pack_for_order(ColorOrder::GRB, v, v, v);
        const ColorOrder all[] = {ColorOrder::RGB, ColorOrder::BGR, ColorOrder::BRG,
                                  ColorOrder::RBG, ColorOrder::GBR};
        for (ColorOrder o : all)
            CHECK_EQ(pack_for_order(o, v, v, v), grb);
    }
}

TEST(nothing_leaks_outside_twenty_four_bits)
{
    // tNeopixel::rgb is a 32-bit field and the top byte is white on RGBW
    // parts. Setting it here would light a white element nobody asked for.
    const ColorOrder all[] = {ColorOrder::GRB, ColorOrder::RGB, ColorOrder::BGR,
                              ColorOrder::BRG, ColorOrder::RBG, ColorOrder::GBR};
    for (ColorOrder o : all)
        CHECK_EQ(pack_for_order(o, 255, 255, 255) & 0xFF000000u, 0u);
}

TEST(packing_is_usable_at_compile_time)
{
    // constexpr, so the render path pays nothing for the indirection.
    static_assert(pack_for_order(ColorOrder::GRB, 0x11, 0x22, 0x33) == 0x00112233u,
                  "GRB packing changed");
    static_assert(pack_for_order(ColorOrder::RGB, 0x11, 0x22, 0x33) == 0x00221133u,
                  "RGB packing changed");
    CHECK(true);
}

TEST(wire_bytes_agree_with_the_packed_word)
{
    // The tie that stops the two presentations drifting. pack_for_order()
    // encodes the neopixel component's convention -- first transmitted byte
    // in bits 15:8, second in 23:16, third in 7:0 -- so reading those three
    // slots out of the word must reproduce the wire order exactly.
    const ColorOrder all[] = {ColorOrder::UNSPECIFIED, ColorOrder::GRB,
                              ColorOrder::RGB, ColorOrder::BGR, ColorOrder::BRG,
                              ColorOrder::RBG, ColorOrder::GBR};
    const uint8_t r = 0x11, g = 0x22, b = 0x33; // distinct, so a swap shows
    for (ColorOrder o : all)
    {
        uint8_t w[3]{};
        bytes_for_order(o, r, g, b, w);
        const uint32_t packed = pack_for_order(o, r, g, b);
        CHECK_EQ(w[0], static_cast<uint8_t>((packed >> 8) & 0xFF));
        CHECK_EQ(w[1], static_cast<uint8_t>((packed >> 16) & 0xFF));
        CHECK_EQ(w[2], static_cast<uint8_t>(packed & 0xFF));
    }
}

TEST(wire_bytes_are_a_permutation_of_the_colour)
{
    // Whatever the order, all three components go out exactly once. A table
    // that dropped one would still look plausible on greys.
    const ColorOrder all[] = {ColorOrder::UNSPECIFIED, ColorOrder::GRB,
                              ColorOrder::RGB, ColorOrder::BGR, ColorOrder::BRG,
                              ColorOrder::RBG, ColorOrder::GBR};
    for (ColorOrder o : all)
    {
        uint8_t w[3]{};
        bytes_for_order(o, 0x11, 0x22, 0x33, w);
        int seen = 0;
        for (uint8_t v : w)
            seen += (v == 0x11) + (v == 0x22) + (v == 0x33);
        CHECK_EQ(seen, 3);
        CHECK(w[0] != w[1] && w[1] != w[2] && w[0] != w[2]);
    }
}

int main() { return ooe::test::run_all(); }
