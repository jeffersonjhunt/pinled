/**
 * @file pinled_color_order.cpp
 * @brief Strip byte order names.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "pinled_color_order.h"

namespace ooe::pinled
{
    const char *color_order_str(ColorOrder o)
    {
        switch (o)
        {
        case ColorOrder::UNSPECIFIED: return "GRB (default)";
        case ColorOrder::GRB:         return "GRB";
        case ColorOrder::RGB:         return "RGB";
        case ColorOrder::BGR:         return "BGR";
        case ColorOrder::BRG:         return "BRG";
        case ColorOrder::RBG:         return "RBG";
        case ColorOrder::GBR:         return "GBR";
        }
        return "unknown";
    }
} // namespace ooe::pinled
