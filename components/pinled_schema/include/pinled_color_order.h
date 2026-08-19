#pragma once

/**
 * @file pinled_color_order.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief Which byte order the LED strip expects, and packing for it.
 * @version 0.4.0
 * @date 2026-08-11
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * @par Why this exists
 * WS2812B is GRB. RGB-ordered clones are common, carry the same markings, and
 * are visually identical — and the failure is quiet, because a near-white tint
 * through a swapped pair is still a near-white tint. An install can be wrong
 * about this indefinitely and only find out when it first displays a saturated
 * colour. That is exactly how it was found here: the first pure blue on the
 * bench came out pink.
 *
 * @par Why the packing is here and not in lamp_map
 * It is arithmetic over three bytes with six cases and an easy off-by-one
 * between "the order the strip wants" and "the order the driver emits". That
 * deserves a table of assertions rather than a squint at a strip, and
 * `lamp_map` cannot be compiled off-target.
 *
 * @par The driver's convention, which is the whole subtlety
 * `neopixel.c` emits, in order, `NP_RGB2GREEN`, `NP_RGB2RED`, `NP_RGB2BLUE` —
 * bits 15:8, then 23:16, then 7:0. So the packed word is not "the colour", it
 * is a *transmission order*, and the byte that goes out first lives in the
 * middle. Getting that backwards yields a permutation that is right for grey
 * and wrong for everything else.
 */

#include <cstdint>

namespace ooe::pinled
{
    /// Values match `pinled_v1_ColorOrder`; asserted in pinled_resolve.cpp.
    /// Unspecified is GRB because that is what every document written before
    /// the field existed already meant.
    enum class ColorOrder : uint8_t
    {
        UNSPECIFIED = 0, ///< GRB
        GRB = 1,
        RGB = 2,
        BGR = 3,
        BRG = 4,
        RBG = 5,
        GBR = 6,
    };

    /**
     * @brief Pack a colour so the driver transmits it in @p order.
     *
     * @param order strip's expected wire order
     * @param r,g,b the colour, in ordinary RGB terms
     * @return a word in the driver's `NP_RGB` convention
     *
     * @note Deliberately returns the driver's packed form rather than calling
     *       `NP_RGB`, so this header stays free of the neopixel component and
     *       can be tested on the host. `lamp_map` assigns the result straight
     *       to `tNeopixel::rgb`, which is the same 24-bit layout.
     */
    constexpr uint32_t pack_for_order(ColorOrder order, uint8_t r, uint8_t g, uint8_t b)
    {
        // The driver sends bits 15:8 first, then 23:16, then 7:0. Naming the
        // three transmitted slots and filling them by name is the only version
        // of this that reads correctly; anything shorter invites the
        // middle-byte mistake back in.
        uint8_t first = g, second = r, third = b; // GRB, and the default

        switch (order)
        {
        case ColorOrder::RGB: first = r; second = g; third = b; break;
        case ColorOrder::BGR: first = b; second = g; third = r; break;
        case ColorOrder::BRG: first = b; second = r; third = g; break;
        case ColorOrder::RBG: first = r; second = b; third = g; break;
        case ColorOrder::GBR: first = g; second = b; third = r; break;
        case ColorOrder::GRB:
        case ColorOrder::UNSPECIFIED:
        default:
            break;
        }

        return (static_cast<uint32_t>(second) << 16) |
               (static_cast<uint32_t>(first) << 8) |
               static_cast<uint32_t>(third);
    }

    /**
     * @brief The same three bytes, in the order they go on the wire.
     *
     * `pack_for_order()` returns a word in the *neopixel component's* packing
     * convention, which is a property of that driver rather than of WS2812.
     * A driver that transmits bytes -- the status pixel's RMT encoder does --
     * wants the order itself, and forcing it through a packed word to unpack
     * it again is how the middle-byte mistake gets back in.
     *
     * One table, two presentations: the host tests assert the two agree for
     * every order, so neither can drift.
     *
     * @param out receives the three bytes, first transmitted first
     */
    constexpr void bytes_for_order(ColorOrder order, uint8_t r, uint8_t g, uint8_t b,
                                   uint8_t out[3])
    {
        uint8_t first = g, second = r, third = b; // GRB, and the default

        switch (order)
        {
        case ColorOrder::RGB: first = r; second = g; third = b; break;
        case ColorOrder::BGR: first = b; second = g; third = r; break;
        case ColorOrder::BRG: first = b; second = r; third = g; break;
        case ColorOrder::RBG: first = r; second = b; third = g; break;
        case ColorOrder::GBR: first = g; second = b; third = r; break;
        case ColorOrder::GRB:
        case ColorOrder::UNSPECIFIED:
        default:
            break;
        }

        out[0] = first;
        out[1] = second;
        out[2] = third;
    }

    /// Human-readable name, for logs.
    const char *color_order_str(ColorOrder o);
} // namespace ooe::pinled
