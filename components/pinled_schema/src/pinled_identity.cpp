/**
 * @file pinled_identity.cpp
 * @brief The rule for an acceptable author handle (FR-REG-1).
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "pinled_identity.h"

#include <cstring>

namespace ooe::pinled
{
    bool author_handle_valid(const char *s)
    {
        if (s == nullptr || s[0] == '\0')
            return false;
        const size_t len = std::strlen(s);
        if (len > kAuthorHandleMax)
            return false;
        if (s[0] == ' ' || s[len - 1] == ' ')
            return false;
        for (size_t i = 0; i < len; ++i)
        {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c < 0x20 || c == 0x7F)
                return false;
        }
        return true;
    }
} // namespace ooe::pinled
