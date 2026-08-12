#pragma once

/**
 * @file rescue.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief Button-held return to SoftAP (FR-UI-7).
 * @version 0.7.0
 * @date 2026-08-12
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * A provisioned device whose network has changed is otherwise unreachable:
 * it joins nothing, so the API is not there, so there is no way to tell it
 * about the new network. Someone replacing a router should not need a USB
 * cable and a laptop to get their lamps back.
 *
 * @par Why a long hold, and what the short press is reserved for
 * `HARDWARE.md` gives GPIO 0 to the profiler re-arm, which step 6 showed we
 * genuinely need — the live monitor reports every channel as OFF because
 * classification only happens at boot. One pin, two jobs, split by duration:
 * a long hold wipes credentials, a short press is left for the re-arm.
 *
 * @par This is not the same as holding BOOT at power-on
 * GPIO 0 low *while resetting* puts the ESP32 in ROM download mode before any
 * of our code runs. This path only exists on a device that is already up. The
 * two are easy to confuse when something does not work, and the symptom
 * differs: download mode is a board that never prints a banner.
 */

#include <cstdint>

#include "esp_err.h"

namespace ooe::pinled
{
    /// Watch the button and wipe credentials when it is held.
    /// @param gpio      button pin, active low with an internal pull-up
    /// @param hold_ms   how long it must be held
    esp_err_t rescue_button_start(int gpio, uint32_t hold_ms);
} // namespace ooe::pinled
