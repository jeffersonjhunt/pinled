#pragma once

/**
 * @file pinled_channel_config.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief The runtime per-channel record, and the projection that builds it.
 * @version 0.3.0
 * @date 2026-08-10
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * @par Two models, and why (FR-CFG-14)
 * The **document model** is the nanopb struct generated from
 * `proto/pinled.proto`. There is deliberately no hand-written parallel class
 * tree: a second definition of the same schema is a second thing to keep in
 * step, which is exactly what having one schema authority (FR-CFG-13) is meant
 * to prevent.
 *
 * The **runtime model** is `ChannelConfig` — flat, POD, sized once at init,
 * and read by the render path every frame. `NFR-4` forbids allocation there,
 * so this type owns no pointers and no strings.
 *
 * @par No lamp names here, on purpose
 * A name is UI-only; nothing in the scan or render path has ever needed one.
 * Keeping them out is most of the point of the split — and it has a pleasant
 * consequence for the API: `GET /profile` can verify the stored file's CRC and
 * stream the payload back **without decoding it at all**, so names never exist
 * as a resident structure on the device.
 *
 * @par The join
 * The profile is keyed by *lamp*; the runtime record is keyed by *channel*.
 * `install.wiring[]` is the join between them, which is what lets a shared
 * profile land correctly on a machine whose modules were chained in a
 * different order (FR-CFG-5/6).
 *
 * @par Inheritance is resolved here
 * A per-lamp `attack_ms` / `decay_ms` / `gain_permille` of zero means "inherit"
 * on the wire. `resolve()` settles every one of them against
 * `InstallConfig.filament`, falling back to `ResolveDefaults`, so nothing
 * downstream ever learns the convention exists.
 */

#include <cstddef>
#include <cstdint>

#include "pinled_drive_class.h"

#include "pinled.pb.h"

namespace ooe::pinled
{
    /**
     * @brief One sensed channel, fully resolved. 14 bytes; 1.8 KB at 128.
     */
    struct ChannelConfig
    {
        uint16_t lamp{0};        ///< lamp number in the machine's matrix; 0 = unbound
        int16_t led_index{-1};   ///< position in the LED string; -1 = unmapped
        uint8_t r{0};            ///< base colour, multiplied by reconstructed brightness
        uint8_t g{0};
        uint8_t b{0};
        DriveClass class_lock{DriveClass::UNKNOWN}; ///< UNKNOWN = the profiler decides
        uint16_t attack_ms{0};     ///< resolved; never 0 after resolve()
        uint16_t decay_ms{0};      ///< resolved; never 0 after resolve()
        uint16_t gain_permille{0}; ///< resolved; 1000 = unity

        /// Bound to a lamp, so a profile entry can style it.
        bool bound() const { return lamp != 0; }
        /// Mapped to an LED, so it can be seen.
        bool mapped() const { return led_index >= 0; }
    };

    /// Used where the install config says nothing. Warm white matches an
    /// unstyled incandescent, so a freshly wired lamp looks like the bulb it
    /// replaced rather than like an error.
    struct ResolveDefaults
    {
        uint16_t attack_ms{30};
        uint16_t decay_ms{40};
        uint16_t gain_permille{1000};
        uint8_t r{255};
        uint8_t g{233};
        uint8_t b{196};
    };

    enum class ResolveStatus
    {
        Ok = 0,
        NullOutput,        ///< out was null with a non-zero capacity
        GeometryTooLarge,  ///< geometry exceeds out_cap, or the 128-channel ceiling
        ChannelOutOfRange, ///< a wiring entry names a channel the geometry does not have
        DuplicateChannel,  ///< two wiring entries claim the same channel
        LedOutOfRange,     ///< led_index is < -1, or >= led_count
        ValueOutOfRange,   ///< a colour, tau or gain that cannot be represented
    };

    /**
     * @brief Project a machine profile and an install config onto channels.
     *
     * Lenient where a half-finished install is normal, strict where a value is
     * ambiguous or impossible:
     *
     * | Case | Behaviour |
     * |---|---|
     * | channel absent from `wiring[]` | **allowed** — unbound and dark; normal mid-commissioning |
     * | wired lamp has no profile entry | **allowed** — bound but unstyled; you wire before you style |
     * | profile entry for an unwired lamp | **ignored** — a shared 60-lamp profile on a 40-lamp install |
     * | same channel twice in `wiring[]` | **rejected** — silently picking one would hide a bad import |
     * | channel or `led_index` out of range | **rejected** — violates the declared geometry |
     * | colour > 255, tau or gain > 65535 | **rejected** — cannot be represented, and clamping would invent data |
     *
     * Every channel in `[0, *channels_out)` is written, including unbound ones,
     * so the caller never reads a stale slot.
     *
     * @param profile      decoded machine profile (may be empty)
     * @param install      decoded install config; supplies geometry and wiring
     * @param out          destination array
     * @param out_cap      capacity of @p out in elements
     * @param channels_out receives the channel count from the geometry
     * @param fallback     used where @p install carries no filament defaults
     *
     * @note O(channels x lamps) — a linear scan per channel, so 16k comparisons
     *       at the 128-channel maximum. This runs at commissioning time, not in
     *       any hot path, and an index would be more state to keep correct than
     *       the scan costs.
     */
    ResolveStatus resolve(const pinled_v1_MachineProfile &profile,
                          const pinled_v1_InstallConfig &install,
                          ChannelConfig *out, size_t out_cap,
                          size_t *channels_out,
                          const ResolveDefaults &fallback = ResolveDefaults{});

    /// Human-readable status, for logs and test failure messages.
    const char *resolve_status_str(ResolveStatus s);

    DriveClass drive_class_from_proto(pinled_v1_DriveClass c);
    pinled_v1_DriveClass drive_class_to_proto(DriveClass c);
} // namespace ooe::pinled
