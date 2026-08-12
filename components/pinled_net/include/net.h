#pragma once

/**
 * @file net.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief Getting the board onto a network, and findable on it.
 * @version 0.5.0
 * @date 2026-08-11
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * @par Two modes, and why both exist now
 * SoftAP is the shipping path: an unprovisioned device raises its own network
 * and a phone joins it (FR-UI-6). Station mode with build-time credentials is
 * a **bring-up stopgap** so the API can be reached from another machine on an
 * existing LAN while it is being written.
 *
 * They are both here at step 5 rather than SoftAP alone because step 7 needs
 * both anyway — provisioning is the act of moving from one to the other — and
 * because a stopgap with no successor tends to become the design.
 *
 * @par The credentials are temporary and compiled in
 * `FR-CFG-12` says Wi-Fi credentials live in NVS and appear in no export at
 * any privacy level. A Kconfig string satisfies neither: it is readable in the
 * binary by anyone holding the board. Step 7 replaces it with
 * `wifi_provisioning`, and `CONFIG_PINLED_WIFI_SSID`/`_PASSWORD` are deleted
 * then. Nothing else should learn to read them.
 *
 * @par No outbound calls, ever
 * `FR-UI-2`: this component brings up an interface and advertises a name. It
 * does not fetch, poll, phone home, or check for updates. The browser is the
 * only participant that talks to both the device and the internet.
 */

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "esp_netif.h"

namespace ooe::pinled
{
    enum class NetMode
    {
        None = 0,
        Station,  ///< joined an existing network (bring-up only)
        SoftAp,   ///< raised our own (FR-UI-6)
    };

    class Net
    {
    public:
        /**
         * @brief Bring up Wi-Fi and mDNS.
         *
         * Station mode when `CONFIG_PINLED_WIFI_SSID` is non-empty, SoftAP
         * otherwise. Returns once the interface is usable — for station mode
         * that means an IP was obtained, or the attempt gave up.
         *
         * @return ESP_OK if an interface came up in either mode.
         *
         * @note Never returns an error for "no credentials". That is the
         *       normal state of an unprovisioned device, not a fault, and
         *       failing boot over it would strand the user with no way to
         *       reach the UI that would fix it.
         */
        esp_err_t start();

        NetMode mode() const { return mode_; }

        /// Dotted-quad address, or "0.0.0.0" before one is held. FR-UI-6 wants
        /// this reported alongside the mDNS name, because mDNS resolution is
        /// the thing most likely to be broken on someone's network.
        const char *ip() const { return ip_; }

        /// Advertised name, without the `.local` suffix.
        const char *hostname() const { return hostname_; }

        /// True once station mode holds an address, or SoftAP is running.
        bool up() const { return up_; }

    private:
        esp_err_t start_station();
        esp_err_t start_softap();
        esp_err_t start_mdns();

        static void on_wifi_event(void *arg, esp_event_base_t base,
                                  int32_t id, void *data);
        static void on_ip_event(void *arg, esp_event_base_t base,
                                int32_t id, void *data);

        NetMode mode_{NetMode::None};
        bool up_{false};
        char ip_[16]{"0.0.0.0"};
        char hostname_[32]{};
        esp_netif_t *netif_{nullptr};
        int retries_{0};
        void *connected_{nullptr}; ///< EventGroupHandle_t, opaque here
    };
} // namespace ooe::pinled
