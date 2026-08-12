#pragma once

/**
 * @file credentials.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief Wi-Fi credentials in NVS, and nowhere else (FR-CFG-12).
 * @version 0.7.0
 * @date 2026-08-12
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * @par Why this is not part of the configuration store
 * `machine_config` owns two documents that are explicitly exportable — that is
 * most of their point, since a machine profile is meant to be shared and an
 * install config is meant to be backed up. Credentials must appear in neither,
 * at any privacy level, which is a promise far easier to keep if there is no
 * field to forget to strip. So they live in NVS, in their own namespace,
 * reachable only through this header.
 *
 * `proto/pinled.proto` says the same thing from the other direction:
 * `InstallConfig` has no credential field and the comment above it says not to
 * add one. Two independent barriers, because "remember not to export this" is
 * the kind of rule that survives exactly as long as the person who wrote it.
 *
 * @par What is stored, and what is not
 * SSID and passphrase, verbatim. **Not encrypted.** NVS encryption needs flash
 * encryption to be meaningful, and without secure boot that is a lock with the
 * key beside it — anyone who can read the flash can read the eFuse-derived key
 * too. What this does buy is that credentials never leave the device in an
 * export, which is the property FR-CFG-12 actually asks for. Treat a board as
 * carrying the credentials of any network it has joined.
 */

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace ooe::pinled
{
    /// 802.11 limits, not choices: 32-byte SSID, 64-byte passphrase, plus NUL.
    inline constexpr size_t kMaxSsid = 33;
    inline constexpr size_t kMaxPassphrase = 65;

    struct WifiCredentials
    {
        char ssid[kMaxSsid]{};
        char password[kMaxPassphrase]{};

        bool valid() const { return ssid[0] != '\0'; }
    };

    /**
     * @brief Read stored credentials.
     *
     * @return ESP_OK when @p out holds usable credentials,
     *         ESP_ERR_NVS_NOT_FOUND when the device has never been
     *         provisioned — which is a normal state, not a fault.
     */
    esp_err_t credentials_load(WifiCredentials &out);

    /// Store credentials, replacing whatever was there.
    esp_err_t credentials_save(const WifiCredentials &in);

    /// Forget them, returning the device to SoftAP on the next boot
    /// (FR-UI-7). Absent is success — the caller asked for them to be gone.
    esp_err_t credentials_erase();

    /// True if the device has been provisioned. Cheaper than a full load when
    /// the answer is all that is wanted.
    bool credentials_present();
} // namespace ooe::pinled
