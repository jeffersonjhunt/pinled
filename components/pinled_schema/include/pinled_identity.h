#pragma once

/**
 * @file pinled_identity.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief The rule for an acceptable author handle (FR-REG-1).
 * @version 0.1.0
 * @date 2026-08-19
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * The usual split: the rule lives here, IDF-free and host-tested; the NVS
 * store that persists the handle (`components/pinled_identity`) and the API
 * endpoint that receives it have nothing worth asserting.
 */

#include <cstddef>

namespace ooe::pinled
{
    /// The longest handle stored or reported, in bytes — one under the
    /// 32-byte schema field (`pinled.options`), which owns this number.
    inline constexpr size_t kAuthorHandleMax = 31;

    /**
     * @brief Is @p s an acceptable author handle?
     *
     * 1 to 31 bytes; no control bytes and no DEL, because the handle is
     * stamped into exported profiles and shown in logs; no leading or
     * trailing space, because an invisible difference between two handles
     * is an attribution bug nobody can see. Bytes >= 0x80 pass: the SPA
     * sends UTF-8 and a name is allowed to have an accent in it —
     * validating encoding here would be borrowing the browser's job.
     *
     * Ownership, uniqueness and verification are cloud-side concerns
     * (FR-REG-1); this rule only keeps garbage out of NVS.
     */
    bool author_handle_valid(const char *s);
} // namespace ooe::pinled
