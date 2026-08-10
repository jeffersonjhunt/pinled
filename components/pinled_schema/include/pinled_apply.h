#pragma once

/**
 * @file pinled_apply.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief Turning a resolved channel record into integrator tuning, and
 *        deciding who wins when the profiler disagrees with it.
 * @version 0.3.0
 * @date 2026-08-10
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * Two sources want to set the same three numbers on every channel: the stored
 * configuration a person edited, and the profiler that classifies what the
 * machine is actually doing. `FR-CFG-8` says one record, written by both, with
 * the class lock deciding precedence — this is that rule, as ordinary logic
 * with ordinary tests rather than a branch buried in `main`.
 *
 * @par The precedence rule
 * 1. **A locked class means the profiler does not touch the channel.** That is
 *    what locking is for: the user has stated what this lamp is, so
 *    reclassifying it every boot would be the bug, not the feature.
 * 2. **An explicitly chosen value always wins**, lock or no lock. Somebody who
 *    asked for a slow fade on one insert keeps it even while the profiler is
 *    otherwise in charge of that channel — otherwise per-lamp presentation
 *    silently stops working at the next boot-time profiling pass.
 * 3. **Everything else comes from the profiler**, which is the whole reason the
 *    product does not ship a hand-tuned table per game.
 *
 * Nothing here touches ESP-IDF or nanopb, so it is host-tested.
 */

#include <cstddef>
#include <cstdint>

#include "pinled_channel_config.h"
#include "pinled_filament_params.h"

namespace ooe::pinled
{
    /// Integrator tuning implied by a channel's stored configuration alone,
    /// ignoring anything the profiler may have learned.
    FilamentParams params_from_config(const ChannelConfig &c);

    /**
     * @brief Combine stored configuration with a profiler result.
     *
     * @param c            the channel's resolved configuration
     * @param from_profiler what the classifier derived for this channel
     * @return the tuning to hand to `Filament::set_params()`
     *
     * See the precedence rule in the file comment. Note that a locked channel
     * still honours explicit per-lamp values — the lock governs *classification*,
     * not the user's own overrides.
     */
    FilamentParams effective_params(const ChannelConfig &c,
                                    const FilamentParams &from_profiler);

    /// True when the profiler's classification may be applied to this channel.
    /// False once a person has stated what the lamp is.
    bool profiler_may_classify(const ChannelConfig &c);

    /**
     * @brief Fill a channel array with the pre-configuration defaults.
     *
     * What the firmware runs on before any stored document exists (FR-CFG-4):
     * identity channel-to-LED mapping, warm white, and the build's default time
     * constants. Deliberately identical to what `LampMap::set_default_mapping()`
     * and `Filament::set_params_all()` already produce, so routing boot through
     * the new path changes no observable behaviour.
     *
     * @param out       destination array
     * @param n         channels to fill
     * @param led_count LED string length; channels beyond it are left unmapped
     * @param d         the defaults to apply
     */
    void default_channels(ChannelConfig *out, size_t n, size_t led_count,
                          const ResolveDefaults &d = ResolveDefaults{});
} // namespace ooe::pinled
