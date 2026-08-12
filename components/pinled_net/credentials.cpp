/**
 * @file credentials.cpp
 * @brief Wi-Fi credentials in NVS, and nowhere else.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "credentials.h"

#include <cstring>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace ooe::pinled
{
    static const char *TAG = "credentials";

    namespace
    {
        // Its own namespace, so a future `nvs_erase_all` on some other
        // subsystem's keys cannot take the network with it.
        constexpr char kNamespace[] = "pinled-wifi";
        constexpr char kKeySsid[] = "ssid";
        constexpr char kKeyPass[] = "pass";
    } // namespace

    esp_err_t credentials_load(WifiCredentials &out)
    {
        nvs_handle_t h;
        esp_err_t err = nvs_open(kNamespace, NVS_READONLY, &h);
        if (err != ESP_OK)
            return err; // NOT_FOUND on a device that has never been provisioned

        size_t len = sizeof(out.ssid);
        err = nvs_get_str(h, kKeySsid, out.ssid, &len);
        if (err == ESP_OK)
        {
            len = sizeof(out.password);
            const esp_err_t perr = nvs_get_str(h, kKeyPass, out.password, &len);
            // An open network stores no passphrase, so a missing password key
            // is a valid provisioning rather than a broken one.
            if (perr == ESP_ERR_NVS_NOT_FOUND)
                out.password[0] = '\0';
            else if (perr != ESP_OK)
                err = perr;
        }
        nvs_close(h);
        return err;
    }

    esp_err_t credentials_save(const WifiCredentials &in)
    {
        if (!in.valid())
            return ESP_ERR_INVALID_ARG;

        nvs_handle_t h;
        esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &h);
        if (err != ESP_OK)
            return err;

        err = nvs_set_str(h, kKeySsid, in.ssid);
        if (err == ESP_OK)
            err = nvs_set_str(h, kKeyPass, in.password);
        if (err == ESP_OK)
            err = nvs_commit(h);
        nvs_close(h);

        // The SSID is logged; the passphrase never is. Anyone holding the board
        // can read both out of flash, so this is not secrecy — it is that a log
        // pasted into a bug report should not carry someone's network password.
        if (err == ESP_OK)
            ESP_LOGI(TAG, "provisioned for \"%s\"", in.ssid);
        else
            ESP_LOGE(TAG, "could not store credentials: %s", esp_err_to_name(err));
        return err;
    }

    esp_err_t credentials_erase()
    {
        nvs_handle_t h;
        esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &h);
        if (err == ESP_ERR_NVS_NOT_FOUND)
            return ESP_OK; // already gone, which is what was asked for
        if (err != ESP_OK)
            return err;

        err = nvs_erase_all(h);
        if (err == ESP_OK)
            err = nvs_commit(h);
        nvs_close(h);

        ESP_LOGW(TAG, "credentials erased; next boot raises the SoftAP");
        return err;
    }

    bool credentials_present()
    {
        WifiCredentials c{};
        return credentials_load(c) == ESP_OK && c.valid();
    }
} // namespace ooe::pinled
