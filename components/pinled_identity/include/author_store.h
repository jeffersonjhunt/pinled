#pragma once

/**
 * @file author_store.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief The author handle in NVS, and nowhere else (FR-REG-1).
 * @version 0.1.0
 * @date 2026-08-19
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * The same shape as `credentials.h`: one small boot-time setting that NVS is
 * actually good at (FR-CFG-7), behind three functions. The handle is pure
 * attribution — the SPA stamps it into exported profiles — so no device
 * function reads it, nothing restarts when it changes, and a device without
 * one is complete. The validity rule lives in `pinled_identity.h`
 * (pinled_schema), where the host can test it; callers apply it before
 * storing.
 */

#include <cstddef>

#include "esp_err.h"

namespace ooe::pinled
{
    /**
     * @brief Read the stored handle into @p out.
     *
     * @p out is always a valid string afterwards: empty when no handle is
     * stored (ESP_ERR_NVS_NOT_FOUND — the ordinary state of a fresh device,
     * not an error worth logging).
     */
    esp_err_t author_load(char *out, size_t cap);

    /// Store @p handle. The caller has already validated it
    /// (`author_handle_valid`); this refuses only what would not fit.
    esp_err_t author_store(const char *handle);

    /// Forget the handle. Clearing an empty store succeeds — the caller
    /// asked for absence and absence is what there is.
    esp_err_t author_erase();
} // namespace ooe::pinled
