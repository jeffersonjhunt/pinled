#pragma once

/**
 * @file pinled_channel_config.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief The runtime per-channel record (FR-CFG-14).
 * @version 0.3.0
 * @date 2026-08-10
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * @par Two models, and why
 * The **document model** is the nanopb struct generated from
 * `proto/pinled.proto`. There is deliberately no hand-written parallel class
 * tree: a second definition of the same schema is a second thing to keep in
 * step, which is what having one schema authority (FR-CFG-13) prevents.
 *
 * This is the **runtime model** — flat, POD, sized once at init, and read by
 * the render path every frame. `NFR-4` forbids allocation there, so it owns no
 * pointers and no strings.
 *
 * @par No nanopb here
 * This header carries no protobuf dependency, so firmware can hold and apply
 * these records before nanopb joins the IDF build. The projection that *builds*
 * them from documents lives in `pinled_resolve.h`, which does need it.
 *
 * @par No lamp names, on purpose
 * A name is UI-only; nothing in scan or render has ever needed one. Keeping
 * them out is most of the point of the split, and it lets `GET /profile`
 * verify the stored CRC and stream the payload back **without decoding it**, so
 * names never exist as a resident structure on the device.
 */

#include <cstddef>
#include <cstdint>

#include "pinled_drive_class.h"

namespace ooe::pinled
{
    /**
     * @brief Which fields a person actually chose, as opposed to inherited.
     *
     * `resolve()` settles every inherit-marker so nothing downstream has to
     * know the convention exists — but settling it destroys the difference
     * between "explicitly 30 ms" and "30 ms because that is the default", and
     * the profiler needs that difference to know what it may overwrite.
     *
     * Without this, a lamp given a deliberately soft fade would have it
     * silently replaced at the next boot-time profiling pass, and the feature
     * would simply not work.
     */
    enum ChannelFlags : uint8_t
    {
        CH_NONE = 0,
        CH_ATTACK_SET = 1u << 0, ///< attack_ms came from the profile, not a default
        CH_DECAY_SET = 1u << 1,
        CH_GAIN_SET = 1u << 2,
        CH_COLOR_SET = 1u << 3, ///< colour came from the profile, not a default
    };

    /**
     * @brief One sensed channel, fully resolved. 16 bytes; 2 KB at 128.
     */
    struct ChannelConfig
    {
        uint16_t lamp{0};      ///< lamp number in the machine's matrix; 0 = unbound
        int16_t led_index{-1}; ///< position in the LED string; -1 = unmapped
        uint8_t r{0};          ///< base colour, multiplied by reconstructed brightness
        uint8_t g{0};
        uint8_t b{0};
        DriveClass class_lock{DriveClass::UNKNOWN}; ///< UNKNOWN = the profiler decides
        uint16_t attack_ms{0};     ///< resolved; never 0 after resolve()
        uint16_t decay_ms{0};      ///< resolved; never 0 after resolve()
        uint16_t gain_permille{0}; ///< resolved; 1000 = unity
        uint8_t flags{CH_NONE};    ///< ChannelFlags: what was chosen vs inherited

        /// Bound to a lamp, so a profile entry can style it.
        bool bound() const { return lamp != 0; }
        /// Mapped to an LED, so it can be seen.
        bool mapped() const { return led_index >= 0; }
        /// The profiler owns this channel's classification.
        bool auto_class() const { return class_lock == DriveClass::UNKNOWN; }

        bool has(ChannelFlags f) const { return (flags & static_cast<uint8_t>(f)) != 0; }
    };

    /// Used where the install config says nothing. Warm white matches an
    /// unstyled incandescent, so a freshly wired lamp looks like the bulb it
    /// replaced rather than like an error.
    ///
    /// @note This is the **only** definition of the default tint.
    /// `LampMap::set_default_mapping()` reads it from here rather than keeping
    /// its own copy — it previously had one, and the two had drifted apart, so
    /// routing boot through the config path would have silently restyled every
    /// unconfigured lamp.
    struct ResolveDefaults
    {
        uint16_t attack_ms{30};
        uint16_t decay_ms{40};
        uint16_t gain_permille{1000};
        uint8_t r{255};
        uint8_t g{200};
        uint8_t b{140};
    };
} // namespace ooe::pinled
