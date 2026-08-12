#pragma once

/**
 * @file dns_hijack.h
 * @brief Answer every DNS query with our own address, for the captive portal.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * Only ever started in SoftAP mode. See the file comment in dns_hijack.cpp for
 * why that restriction is not negotiable.
 */

#include <cstdint>

#include "esp_err.h"

namespace ooe::pinled
{
    /// Start the hijacking resolver on port 53.
    /// @param ip_be the address to answer with, in network byte order.
    esp_err_t dns_hijack_start(uint32_t ip_be);
} // namespace ooe::pinled
