#pragma once

/**
 * @file pinled_resolve.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief Projection of the two configuration documents onto per-channel state.
 * @version 0.3.0
 * @date 2026-08-10
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * Split from `pinled_channel_config.h` because this half needs the generated
 * nanopb header and that half must not: firmware holds and applies
 * `ChannelConfig` records today, while nanopb does not join the IDF build
 * until the store lands (`FIRMWARE_PLAN.md` §5.1 step 4).
 *
 * @par The join
 * The profile is keyed by *lamp*; the runtime record is keyed by *channel*.
 * `install.wiring[]` is the join, which is what lets a shared profile land
 * correctly on a machine whose modules were chained in a different order
 * (FR-CFG-5/6).
 */

#include <cstddef>
#include <cstdint>

#include "pinled_channel_config.h"

#include "pinled.pb.h"

namespace ooe::pinled
{
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
     * so the caller never reads a stale slot. Values are validated before
     * anything is written, so a rejected document leaves `out` in one state
     * rather than half-updated.
     *
     * `ChannelFlags` record which values a person actually chose, because
     * settling the inherit-markers otherwise destroys that information and the
     * profiler needs it (see `pinled_apply.h`).
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
