/**
 * @file author_store.cpp
 * @brief The author handle in NVS, and nowhere else (FR-REG-1).
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "author_store.h"

#include <cstring>

#include "esp_log.h"
#include "nvs.h"

#include "pinled_identity.h"

namespace ooe::pinled
{
    static const char *TAG = "identity";

    namespace
    {
        // Its own namespace, same reasoning as pinled-wifi: an erase aimed at
        // some other subsystem's keys must not take the attribution with it —
        // and the network-forget rescue must not take the handle.
        constexpr char kNamespace[] = "pinled-id";
        constexpr char kKeyAuthor[] = "author";
    } // namespace

    esp_err_t author_load(char *out, size_t cap)
    {
        if (out == nullptr || cap == 0)
            return ESP_ERR_INVALID_ARG;
        out[0] = '\0';

        nvs_handle_t h;
        esp_err_t err = nvs_open(kNamespace, NVS_READONLY, &h);
        if (err != ESP_OK)
            return err; // NOT_FOUND on a device that never stored one

        size_t len = cap;
        err = nvs_get_str(h, kKeyAuthor, out, &len);
        nvs_close(h);
        if (err != ESP_OK)
            out[0] = '\0';
        return err;
    }

    esp_err_t author_store(const char *handle)
    {
        if (handle == nullptr || std::strlen(handle) > kAuthorHandleMax)
            return ESP_ERR_INVALID_ARG;

        nvs_handle_t h;
        esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &h);
        if (err != ESP_OK)
            return err;

        err = nvs_set_str(h, kKeyAuthor, handle);
        if (err == ESP_OK)
            err = nvs_commit(h);
        nvs_close(h);

        if (err == ESP_OK)
            ESP_LOGI(TAG, "author handle stored: \"%s\"", handle);
        else
            ESP_LOGE(TAG, "could not store the handle: %s", esp_err_to_name(err));
        return err;
    }

    esp_err_t author_erase()
    {
        nvs_handle_t h;
        esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &h);
        if (err == ESP_ERR_NVS_NOT_FOUND)
            return ESP_OK; // already gone, which is what was asked for
        if (err != ESP_OK)
            return err;

        err = nvs_erase_key(h, kKeyAuthor);
        if (err == ESP_ERR_NVS_NOT_FOUND)
            err = ESP_OK;
        if (err == ESP_OK)
            err = nvs_commit(h);
        nvs_close(h);

        if (err == ESP_OK)
            ESP_LOGI(TAG, "author handle cleared");
        return err;
    }
} // namespace ooe::pinled
