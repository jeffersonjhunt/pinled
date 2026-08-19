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
 * @par Why a long hold, and what the short press does
 * `HARDWARE.md` gives GPIO 0 to the profiler re-arm as well, which step 6
 * showed we genuinely need — the live monitor reported every channel as OFF
 * because classification only happened at boot. One pin, two jobs, split by
 * duration: a long hold wipes credentials, a short press calls
 * @p on_short_press.
 *
 * One task owns the pin and decides which happened, rather than two tasks
 * polling it and each forming their own opinion. The alternative was a second
 * Kconfig GPIO and a second debounce, which is two answers to "was it
 * pressed" and no way to make them agree.
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
    /// Live reporting of what the button is doing, as distinct from what it
    /// will eventually mean.
    ///
    /// FR-IND-7 is the reason this exists separately from @p on_short_press:
    /// the acknowledgement has to fire the instant a press is *accepted*, not
    /// on release when the outcome is finally known, because a press whose
    /// only feedback is its outcome is indistinguishable from a press that
    /// never registered. That ambiguity cost a bench session on 2026-08-14.
    ///
    /// Both callbacks run on the button task, so neither may block.
    struct ButtonFeedback
    {
        /// The press just passed the debounce threshold. Fires once per press,
        /// before anything has decided what the press is for.
        void (*accepted)(void *arg){nullptr};

        /// How long the button has been held, every poll while it is down,
        /// and once with 0 on release. FR-IND-8 needs both halves: the rising
        /// count drives the erase countdown and the 0 returns the indicator to
        /// whatever it was showing before.
        void (*held)(void *arg, uint32_t held_ms){nullptr};

        void *arg{nullptr};
    };

    /// Watch the button: wipe credentials when it is held, and report a short
    /// press to the caller.
    ///
    /// @param gpio           button pin, active low with an internal pull-up
    /// @param hold_ms        how long it must be held to erase the network
    /// @param on_short_press called on release after a press shorter than
    ///                       @p hold_ms; null leaves a short press logged and
    ///                       ignored. Runs on the button task, so it must not
    ///                       block — post a request and return.
    /// @param arg            passed through to @p on_short_press
    /// @param feedback       optional live press/hold reporting; copied, so
    ///                       the caller's struct need not outlive the call
    esp_err_t rescue_button_start(int gpio, uint32_t hold_ms,
                                  void (*on_short_press)(void *) = nullptr,
                                  void *arg = nullptr,
                                  const ButtonFeedback *feedback = nullptr);
} // namespace ooe::pinled
