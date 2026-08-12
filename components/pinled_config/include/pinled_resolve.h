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
#include "pinled_machine_config.h"

#include "pinled.pb.h"

namespace ooe::pinled
{
    /**
     * @brief How much of a channel's LED string this build will accept.
     *
     * `led_count` decides an allocation in `LampMap::init()`, so an absurd
     * value in a corrupt document is a boot-time out-of-memory rather than a
     * rejected document. 1024 is far past any real playfield — at 30 mA a LED
     * that is 30 A of string — and exists only to bound the arithmetic.
     */
    inline constexpr uint32_t kMaxLedCount = 1024;

    enum class InstallStatus
    {
        Ok = 0,
        GeometryTooLarge, ///< more than 128 channels, or led_count past kMaxLedCount
        PinOutOfRange,    ///< a pin number no GPIO could have
        ValueOutOfRange,  ///< a rate, mode or gamma that cannot be honoured
    };

    /**
     * @brief Project an install document onto the device-wide runtime record.
     *
     * @par What "unset" means, once, for all of it
     * proto3 gives scalars no presence: a field left out and a field set to
     * zero are the same bytes. So the granularity of "specified" is the
     * *submessage* — `has_geometry`, `has_pins`, `has_scan`, `has_render`,
     * `has_filament`. An absent submessage inherits from @p fallback entirely.
     *
     * Within a submessage that IS present, the rule splits on whether zero is a
     * legal value for that particular field:
     *
     * | Field kind | Zero means | Examples |
     * |---|---|---|
     * | counts, rates, gamma | inherit — zero is impossible as a real value | `num_modules`, `sample_rate_hz`, `refresh_hz`, `gamma_x100` |
     * | modes, flags, pins | itself — zero is a legal setting | `spi_mode` (0 is a real SPI mode), `active_low`, GPIO 0 |
     *
     * @par The one special case, and why it is not arbitrary
     * A `Pins` message whose four pins are ALL zero is treated as unspecified
     * rather than "put everything on GPIO 0". Four signals cannot share one
     * pin, so that document cannot be legitimate — whereas a writer that
     * emitted `Pins` without filling it in is entirely plausible. Honouring it
     * literally would drive a strapping pin at boot, which is the one failure
     * here with a hardware consequence.
     *
     * @param install   decoded install document
     * @param out       receives the projection
     * @param fallback  used for everything the document does not specify;
     *                  normally the Kconfig build defaults (FR-CFG-4)
     *
     * @note Nothing is written to @p out until every field has been validated,
     *       so a rejected document leaves the caller's defaults intact rather
     *       than a half-applied mixture.
     */
    InstallStatus install_to_machine(const pinled_v1_InstallConfig &install,
                                     MachineConfig &out,
                                     const MachineConfig &fallback = MachineConfig{});

    /// Human-readable status, for logs and test failure messages.
    const char *install_status_str(InstallStatus s);

    /**
     * @brief Describe the running configuration as an install document.
     *
     * The reverse of `install_to_machine()`, and **deliberately lossy**. The
     * runtime records do not carry everything a document can — there is no
     * device-wide gain, no brightness cap, no lamp names, and nothing at all
     * from a newer schema this build does not understand.
     *
     * That lossiness is the whole argument for how `GET /api/v1/config`
     * behaves: when a document is stored it is streamed back **verbatim**,
     * never re-encoded, because re-encoding would silently drop every field
     * this firmware cannot represent — which is exactly what unknown-field
     * preservation exists to prevent (FR-UI-4). This function is only for the
     * case where nothing is stored and the UI still needs to know what the
     * device is actually running.
     *
     * @param m         the running device-wide record
     * @param channels  the running per-channel records; may be null
     * @param count     number of entries in @p channels
     * @param out       receives the document; fully overwritten
     *
     * @note Emits a wiring entry only for channels that are bound or mapped.
     *       A channel that is neither is absent from the document, which is
     *       what `resolve()` already treats as the default.
     */
    void machine_to_install(const MachineConfig &m,
                            const ChannelConfig *channels, size_t count,
                            pinled_v1_InstallConfig &out);

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
