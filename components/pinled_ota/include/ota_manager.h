#pragma once

/**
 * @file ota_manager.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief OTA staging: stream, verify, wait for the button (FR-OTA-1/2/3/6/7).
 * @version 0.1.0
 * @date 2026-08-18
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * The thin half, same split as the indicator: every rule — which transitions
 * are legal, who wins the confirm/expiry race, what the 49-day wrap does to a
 * window — lives in `pinled_ota_state.h` and is tested on the host. This
 * class owns the flash handle, the SHA-256 running over the received bytes,
 * the expiry timer, and the restart.
 *
 * @par Who calls what, from where
 *   httpd task     begin() / write() / finish() / abort_upload() / discard()
 *   button task    confirm()
 *   esp_timer task the expiry callback
 * None of them takes a lock. The staging word is the baton, exactly as in the
 * profiler hand-off: every transition is a compare-exchange, so the losing
 * side of any race sees its exchange fail and gives up, rather than half
 * applying. The upload path itself (begin -> write -> finish) is single-file
 * because httpd runs one request at a time; the word only has to referee
 * between the upload, the button, the timer, and a second uploader being
 * told no (FR-OTA-2).
 *
 * @par There is no API confirm, on purpose
 * confirm() exists for the button task alone. Physical presence is the
 * entire authorisation (FR-OTA-2): the API can stage and the API can
 * discard — the harmless directions — but only a hand on the board can make
 * it boot.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"

#include "pinled_ota_state.h"

namespace ooe::pinled
{
    /// What the OTA machine tells the rest of the firmware, primarily so the
    /// indicator can show the window (FR-IND-3) without this component ever
    /// depending on it. All callbacks are copied at init and may be null;
    /// they run on whichever task drove the transition, so they must not
    /// block — post and return, like ButtonFeedback.
    struct OtaEvents
    {
        /// An image is staged and the confirmation window just opened.
        void (*staged)(void *arg, uint32_t window_ms){nullptr};

        /// The staging ended without an apply: expiry, discard, or a failed
        /// upload after `staged` was never reached. Fires only if `staged`
        /// fired for this generation.
        void (*cleared)(void *arg){nullptr};

        /// Confirmed. The boot partition is switched and the restart is
        /// scheduled; this is the moment to show APPLYING.
        void (*applying)(void *arg){nullptr};

        void *arg{nullptr};
    };

    class OtaManager
    {
    public:
        /// @param confirm_window_ms how long a staged image stays confirmable
        /// @param events            copied; see OtaEvents
        esp_err_t init(uint32_t confirm_window_ms, const OtaEvents *events);

        // --- the upload path (httpd task) --------------------------------

        /// Claim the machine and open the inactive slot. Refused unless IDLE
        /// (FR-OTA-2): a second upload during another upload, a pending
        /// window, or the restart gap gets ESP_ERR_INVALID_STATE and the
        /// caller turns that into an HTTP 409.
        /// @param image_size from Content-Length, so the erase is sized once
        esp_err_t begin(size_t image_size);

        /// Append received bytes: flash write plus the running SHA-256.
        esp_err_t write(const void *data, size_t len);

        /// Close the slot and stage. esp_ota_end() validates the image
        /// structure; if @p expected_sha256 (32 bytes) is non-null the
        /// digest of the received bytes must match it exactly (FR-OTA-3's
        /// device-side backstop — the browser checks the published hash
        /// before upload, this catches what transit did to it after).
        /// On success the word goes PENDING, the expiry timer is armed, and
        /// events.staged fires.
        esp_err_t finish(const uint8_t *expected_sha256);

        /// Upload failed partway: release the flash handle and return to
        /// IDLE. Safe to call whether or not begin() succeeded.
        void abort_upload();

        // --- the pending window -------------------------------------------

        /// Discard a staged image (httpd task, DELETE /ota — the safe
        /// direction, so the API may do it). True if there was one.
        bool discard();

        /// The button (FR-OTA-7). True if a staged image was confirmed:
        /// the boot partition is switched, events.applying fires, and the
        /// restart is scheduled. False from any other phase — including a
        /// press that lost the race with expiry — and the caller falls
        /// through to the press's ordinary meaning.
        bool confirm();

        // --- introspection (any task) --------------------------------------

        OtaPhase phase() const { return ota_phase(word_.load(std::memory_order_relaxed)); }
        uint32_t remaining_ms() const;

        /// FR-OTA-6, called once the API is up: if this boot is the first of
        /// a new image, tell the bootloader it may keep it. Static because it
        /// concerns the RUNNING image, not any staging.
        static void mark_running_valid();

    private:
        static void expiry_cb(void *arg);
        void clear_after(uint32_t expect_word);
        static uint32_t now_ms();

        std::atomic<uint32_t> word_{ota_pack(OtaPhase::IDLE, 0)};
        std::atomic<uint32_t> deadline_ms_{0};

        uint32_t window_ms_{30000};
        OtaEvents events_{};

        // Upload state. Touched only between a successful begin() and the
        // finish()/abort_upload() that ends it, all on the httpd task.
        uint32_t ota_handle_{0}; ///< esp_ota_handle_t, kept opaque here
        const void *slot_{nullptr}; ///< esp_partition_t*, opaque likewise
        size_t received_{0};
        size_t expected_size_{0};
        void *sha_{nullptr}; ///< mbedtls_sha256_context*, opaque likewise

        void *expiry_timer_{nullptr}; ///< esp_timer_handle_t
    };
} // namespace ooe::pinled
