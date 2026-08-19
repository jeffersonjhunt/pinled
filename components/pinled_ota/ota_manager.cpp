/**
 * @file ota_manager.cpp
 * @brief OTA staging: stream, verify, wait for the button (FR-OTA-1/2/3/6/7).
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "ota_manager.h"

#include <cstring>
#include <new>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "mbedtls/sha256.h"

namespace ooe::pinled
{
    static const char *TAG = "ota";

    namespace
    {
        /// Long enough for the HTTP response and the log to leave, short
        /// enough that the person watching the indicator sees APPLYING as a
        /// beat and not a hang. Same figure the config apply path uses.
        constexpr uint64_t kRestartDelayUs = 500 * 1000;

        void schedule_restart()
        {
            esp_timer_create_args_t args{};
            args.callback = [](void *) {
                ESP_LOGW(TAG, "restarting into the staged image");
                esp_restart();
            };
            args.name = "ota-restart";
            esp_timer_handle_t t = nullptr;
            if (esp_timer_create(&args, &t) == ESP_OK)
                esp_timer_start_once(t, kRestartDelayUs);
            else
                esp_restart(); // the blunt fallback beats a confirmed image never booting
        }
    } // namespace

    uint32_t OtaManager::now_ms()
    {
        return static_cast<uint32_t>(esp_timer_get_time() / 1000);
    }

    esp_err_t OtaManager::init(uint32_t confirm_window_ms, const OtaEvents *events)
    {
        window_ms_ = confirm_window_ms;
        if (events)
            events_ = *events;

        esp_timer_create_args_t args{};
        args.callback = expiry_cb;
        args.arg = this;
        args.name = "ota-expiry";
        esp_err_t err = esp_timer_create(&args, reinterpret_cast<esp_timer_handle_t *>(&expiry_timer_));
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "expiry timer: %s", esp_err_to_name(err));
            return err;
        }

        const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
        if (next == nullptr)
        {
            // A single-slot table. Everything else still works; OTA reports
            // itself unavailable instead of failing at the first upload.
            ESP_LOGW(TAG, "no inactive OTA slot in this partition table; "
                          "uploads will be refused");
            return ESP_OK;
        }
        ESP_LOGI(TAG, "inactive slot %s at 0x%06x (%u KB), confirm window %u ms",
                 next->label, (unsigned)next->address, (unsigned)(next->size / 1024),
                 (unsigned)window_ms_);
        return ESP_OK;
    }

    esp_err_t OtaManager::begin(size_t image_size)
    {
        const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
        if (next == nullptr)
            return ESP_ERR_NOT_SUPPORTED;
        if (image_size == 0 || image_size > next->size)
            return ESP_ERR_INVALID_SIZE;

        // Claim the machine first, flash second: the refusal path must not
        // depend on how far a competing upload happens to have gotten.
        uint32_t w = word_.load(std::memory_order_acquire);
        for (;;)
        {
            const uint32_t next_w = ota_on_begin(w);
            if (next_w == w)
            {
                ESP_LOGW(TAG, "upload refused: %s", ota_phase_name(ota_phase(w)));
                return ESP_ERR_INVALID_STATE;
            }
            if (word_.compare_exchange_weak(w, next_w, std::memory_order_acq_rel))
                break;
        }

        esp_ota_handle_t handle = 0;
        const esp_err_t err = esp_ota_begin(next, image_size, &handle);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
            uint32_t cur = word_.load(std::memory_order_acquire);
            while (ota_on_abandon(cur) != cur &&
                   !word_.compare_exchange_weak(cur, ota_on_abandon(cur),
                                                std::memory_order_acq_rel))
            {
            }
            return err;
        }

        auto *sha = new (std::nothrow) mbedtls_sha256_context;
        if (sha == nullptr)
        {
            esp_ota_abort(handle);
            abort_upload();
            return ESP_ERR_NO_MEM;
        }
        mbedtls_sha256_init(sha);
        if (mbedtls_sha256_starts(sha, 0 /* SHA-256, not 224 */) != 0)
        {
            delete sha;
            esp_ota_abort(handle);
            abort_upload();
            return ESP_FAIL;
        }

        slot_ = next;
        ota_handle_ = handle;
        sha_ = sha;
        received_ = 0;
        expected_size_ = image_size;
        ESP_LOGI(TAG, "receiving %u bytes into %s", (unsigned)image_size, next->label);
        return ESP_OK;
    }

    esp_err_t OtaManager::write(const void *data, size_t len)
    {
        if (ota_phase(word_.load(std::memory_order_relaxed)) != OtaPhase::RECEIVING)
            return ESP_ERR_INVALID_STATE;
        const esp_err_t err = esp_ota_write(ota_handle_, data, len);
        if (err != ESP_OK)
            return err;
        if (mbedtls_sha256_update(static_cast<mbedtls_sha256_context *>(sha_),
                                  static_cast<const unsigned char *>(data), len) != 0)
            return ESP_FAIL; // the handler aborts the upload on any write error
        received_ += len;
        return ESP_OK;
    }

    esp_err_t OtaManager::finish(const uint8_t *expected_sha256)
    {
        if (ota_phase(word_.load(std::memory_order_relaxed)) != OtaPhase::RECEIVING)
            return ESP_ERR_INVALID_STATE;

        uint8_t digest[32]{};
        const bool digest_ok =
            mbedtls_sha256_finish(static_cast<mbedtls_sha256_context *>(sha_), digest) == 0;
        if (!digest_ok)
        {
            abort_upload();
            return ESP_FAIL;
        }

        if (received_ != expected_size_)
        {
            ESP_LOGE(TAG, "short upload: %u of %u bytes",
                     (unsigned)received_, (unsigned)expected_size_);
            abort_upload();
            return ESP_ERR_INVALID_SIZE;
        }

        if (expected_sha256 && std::memcmp(digest, expected_sha256, sizeof(digest)) != 0)
        {
            // The browser checked the published hash before uploading
            // (FR-OTA-3); this catches what happened to the bytes since.
            ESP_LOGE(TAG, "SHA-256 mismatch: the received image is not the published one");
            abort_upload();
            return ESP_ERR_INVALID_CRC;
        }

        // Validates the image structure — magic, segments, app descriptor —
        // so a truncated or non-firmware upload can never reach the
        // confirmation (FR-OTA-2).
        const esp_err_t err = esp_ota_end(ota_handle_);
        ota_handle_ = 0;
        delete static_cast<mbedtls_sha256_context *>(sha_);
        sha_ = nullptr;
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
            uint32_t cur = word_.load(std::memory_order_acquire);
            while (ota_on_abandon(cur) != cur &&
                   !word_.compare_exchange_weak(cur, ota_on_abandon(cur),
                                                std::memory_order_acq_rel))
            {
            }
            return err;
        }

        // Deadline before phase: the moment the word reads PENDING, the
        // button may confirm, and it must never confirm against a stale
        // deadline from the previous staging.
        deadline_ms_.store(now_ms() + window_ms_, std::memory_order_release);

        uint32_t w = word_.load(std::memory_order_acquire);
        for (;;)
        {
            const uint32_t next_w = ota_on_staged(w);
            if (next_w == w)
                return ESP_ERR_INVALID_STATE; // cannot happen; belt anyway
            if (word_.compare_exchange_weak(w, next_w, std::memory_order_acq_rel))
                break;
        }

        esp_timer_stop(static_cast<esp_timer_handle_t>(expiry_timer_)); // no-op if idle
        esp_timer_start_once(static_cast<esp_timer_handle_t>(expiry_timer_),
                             static_cast<uint64_t>(window_ms_) * 1000);

        ESP_LOGW(TAG, "image staged in %s; press the button within %u s to boot it",
                 static_cast<const esp_partition_t *>(slot_)->label,
                 (unsigned)(window_ms_ / 1000));
        if (events_.staged)
            events_.staged(events_.arg, window_ms_);
        return ESP_OK;
    }

    void OtaManager::abort_upload()
    {
        if (ota_handle_ != 0)
        {
            esp_ota_abort(ota_handle_);
            ota_handle_ = 0;
        }
        if (sha_ != nullptr)
        {
            delete static_cast<mbedtls_sha256_context *>(sha_);
            sha_ = nullptr;
        }
        uint32_t w = word_.load(std::memory_order_acquire);
        for (;;)
        {
            if (ota_phase(w) != OtaPhase::RECEIVING)
                return; // nothing of ours left to clean up
            if (word_.compare_exchange_weak(w, ota_on_abandon(w),
                                            std::memory_order_acq_rel))
                return;
        }
    }

    bool OtaManager::discard()
    {
        uint32_t w = word_.load(std::memory_order_acquire);
        for (;;)
        {
            if (ota_phase(w) != OtaPhase::PENDING)
                return false;
            if (word_.compare_exchange_weak(w, ota_on_abandon(w),
                                            std::memory_order_acq_rel))
                break;
        }
        esp_timer_stop(static_cast<esp_timer_handle_t>(expiry_timer_));
        ESP_LOGI(TAG, "staged image discarded by request");
        if (events_.cleared)
            events_.cleared(events_.arg);
        return true;
    }

    bool OtaManager::confirm()
    {
        uint32_t w = word_.load(std::memory_order_acquire);
        for (;;)
        {
            const uint32_t next_w =
                ota_on_confirm(w, now_ms(), deadline_ms_.load(std::memory_order_acquire));
            if (next_w == w)
                return false; // not pending, or the expiry won the race
            if (word_.compare_exchange_weak(w, next_w, std::memory_order_acq_rel))
                break;
        }
        esp_timer_stop(static_cast<esp_timer_handle_t>(expiry_timer_));

        const esp_err_t err =
            esp_ota_set_boot_partition(static_cast<const esp_partition_t *>(slot_));
        if (err != ESP_OK)
        {
            // The one transition APPLYING may make: back out, so the machine
            // is not wedged forever on a flash error nobody can see.
            ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
            word_.store(ota_pack(OtaPhase::IDLE,
                                 ota_gen(word_.load(std::memory_order_relaxed))),
                        std::memory_order_release);
            if (events_.cleared)
                events_.cleared(events_.arg);
            return false;
        }

        ESP_LOGW(TAG, "confirmed by button: booting %s next",
                 static_cast<const esp_partition_t *>(slot_)->label);
        if (events_.applying)
            events_.applying(events_.arg);
        schedule_restart();
        return true;
    }

    uint32_t OtaManager::remaining_ms() const
    {
        if (ota_phase(word_.load(std::memory_order_relaxed)) != OtaPhase::PENDING)
            return 0;
        return ota_remaining_ms(now_ms(), deadline_ms_.load(std::memory_order_relaxed));
    }

    void OtaManager::expiry_cb(void *arg)
    {
        auto *self = static_cast<OtaManager *>(arg);
        uint32_t w = self->word_.load(std::memory_order_acquire);
        for (;;)
        {
            // Only a PENDING word whose deadline is genuinely behind us is
            // retired. A discard or a fresh upload that beat this callback
            // changed the word, the compare-exchange misses, and the stale
            // timer does nothing — the case test_ota_state stages on the host.
            if (ota_phase(w) != OtaPhase::PENDING ||
                !ota_expired(now_ms(), self->deadline_ms_.load(std::memory_order_acquire)))
                return;
            if (self->word_.compare_exchange_weak(w, ota_on_abandon(w),
                                                  std::memory_order_acq_rel))
                break;
        }
        ESP_LOGW(TAG, "confirmation window expired; staged image discarded");
        if (self->events_.cleared)
            self->events_.cleared(self->events_.arg);
    }

    void OtaManager::mark_running_valid()
    {
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_ota_img_states_t state{};
        if (esp_ota_get_state_partition(running, &state) != ESP_OK)
            return;
        if (state != ESP_OTA_IMG_PENDING_VERIFY)
            return;
        // FR-OTA-6: this boot is the first of a freshly applied image, and it
        // got far enough to serve the API — which is the definition of
        // working. Tell the bootloader to keep it.
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
            ESP_LOGW(TAG, "first boot of %s verified: rollback cancelled, image kept",
                     running->label);
    }
} // namespace ooe::pinled
