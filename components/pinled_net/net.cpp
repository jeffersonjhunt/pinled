/**
 * @file net.cpp
 * @brief Wi-Fi station or SoftAP, plus mDNS.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "net.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "dns_hijack.h"
#include "mdns.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

namespace ooe::pinled
{
    static const char *TAG = "net";

    namespace
    {
        constexpr int kConnectedBit = BIT0;
        constexpr int kFailedBit = BIT1;

        /// Give up rather than retry forever. A wrong password should reach the
        /// log in seconds, not keep the boot path hostage — and on the shipping
        /// path there is a SoftAP to fall back to.
        constexpr int kMaxRetries = 6;

        /// Long enough for DHCP on a slow router, short enough that a typo does
        /// not look like a hang.
        constexpr TickType_t kJoinTimeout = pdMS_TO_TICKS(20000);

        /// NVS is now the only source. The build-time credentials that got
        /// step 5 onto a network are gone, as their own help text promised:
        /// FR-CFG-12 wants credentials in NVS and absent from every export,
        /// and a compiled-in string was readable in any image built from this
        /// tree. An unprovisioned device raises the SoftAP instead, which is
        /// the shipping behaviour and no longer a fallback.
        bool resolve_credentials(WifiCredentials &out)
        {
            return credentials_load(out) == ESP_OK && out.valid();
        }
    } // namespace

    void Net::on_wifi_event(void *arg, esp_event_base_t, int32_t id, void *)
    {
        Net *self = static_cast<Net *>(arg);
        auto group = static_cast<EventGroupHandle_t>(self->connected_);

        switch (id)
        {
        case WIFI_EVENT_STA_START:
            // Only when we actually mean to join something. In SoftAP mode the
            // station interface exists purely so a scan is legal (APSTA), and
            // connecting it with an empty config puts the driver in a
            // "connecting" state that refuses scans outright — which presented
            // as an empty network list on the setup page.
            if (self->want_sta_connect_)
                esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            if (self->retries_ < kMaxRetries)
            {
                ++self->retries_;
                ESP_LOGW(TAG, "disconnected; retry %d of %d", self->retries_, kMaxRetries);
                esp_wifi_connect();
            }
            else
            {
                ESP_LOGE(TAG, "could not join after %d attempts", kMaxRetries);
                if (group)
                    xEventGroupSetBits(group, kFailedBit);
            }
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "a client joined the SoftAP");
            break;
        default:
            break;
        }
    }

    void Net::on_ip_event(void *arg, esp_event_base_t, int32_t id, void *data)
    {
        if (id != IP_EVENT_STA_GOT_IP)
            return;

        Net *self = static_cast<Net *>(arg);
        auto *event = static_cast<ip_event_got_ip_t *>(data);
        std::snprintf(self->ip_, sizeof(self->ip_), IPSTR, IP2STR(&event->ip_info.ip));
        self->retries_ = 0;

        auto group = static_cast<EventGroupHandle_t>(self->connected_);
        if (group)
            xEventGroupSetBits(group, kConnectedBit);
    }

    esp_err_t Net::start()
    {
        std::snprintf(hostname_, sizeof(hostname_), "%s", CONFIG_PINLED_HOSTNAME);

        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());

        WifiCredentials creds{};
        const bool provisioned = resolve_credentials(creds);

        const esp_err_t err = provisioned ? start_station(creds) : start_softap();
        if (err != ESP_OK)
            return err;

        start_mdns(); // advisory: a board without mDNS is still reachable by IP

        // FR-UI-6 wants BOTH reported. mDNS is the thing most likely to be
        // broken on someone's network — corporate DNS, a VLAN, an OS that
        // dropped Bonjour — and when it is, the numeric address in this line
        // is the only way anyone gets to the UI.
        ESP_LOGI(TAG, "reachable at http://%s.local/ and http://%s/",
                 hostname_, ip_);
        return ESP_OK;
    }

    esp_err_t Net::start_station(const WifiCredentials &creds)
    {
        want_sta_connect_ = true;

        auto group = xEventGroupCreate();
        connected_ = group;
        netif_ = esp_netif_create_default_wifi_sta();

        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&init));

        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &Net::on_wifi_event, this, nullptr));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &Net::on_ip_event, this, nullptr));

        // memcpy, not snprintf: the driver's fields are fixed 32/64-byte
        // arrays that are NOT NUL-terminated when full, so a 32-character SSID
        // — perfectly legal — has no room for the terminator snprintf insists
        // on writing. cfg is zero-initialised, so a short name is padded.
        wifi_config_t cfg{};
        const size_t ssid_len =
            std::min(std::strlen(creds.ssid), sizeof(cfg.sta.ssid));
        const size_t pass_len =
            std::min(std::strlen(creds.password), sizeof(cfg.sta.password));
        std::memcpy(cfg.sta.ssid, creds.ssid, ssid_len);
        std::memcpy(cfg.sta.password, creds.password, pass_len);

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
        ESP_ERROR_CHECK(esp_wifi_start());

        // No modem sleep. The default is WIFI_PS_MIN_MODEM, which parks the
        // radio between beacons and hands the AP the job of buffering our
        // traffic -- fine for a battery sensor, wrong for a mains-powered
        // controller whose entire job while someone is commissioning it is to
        // answer promptly. It also interacts badly with a 10 kHz scan task
        // that owns core 1.
        //
        // Measured on the bench 2026-08-11, same board, same AP, one line of
        // difference: with modem sleep on, 9 of 10 pings were lost and the API
        // was unusable; with it off, 15 of 15 at 2.9 ms average. Nothing here
        // is saving power that matters — the LED string dwarfs the radio.
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

        // The SSID is logged and the password is not. Anyone with the board can
        // read both out of the flash, so this is tidiness rather than security
        // — but a log that gets pasted into a bug report should not carry it.
        ESP_LOGI(TAG, "joining \"%s\"...", creds.ssid);

        const EventBits_t bits = xEventGroupWaitBits(
            group, kConnectedBit | kFailedBit, pdFALSE, pdFALSE, kJoinTimeout);

        if (bits & kConnectedBit)
        {
            mode_ = NetMode::Station;
            up_ = true;
            return ESP_OK;
        }

        // A provisioned device that cannot reach its network falls back to
        // the SoftAP rather than sitting unreachable (FR-UI-6). Someone whose
        // router was replaced needs a way in, and the alternative is a board
        // that can only be recovered with a USB cable.
        ESP_LOGE(TAG, "%s — falling back to the SoftAP so the device can be reached",
                 (bits & kFailedBit) ? "join failed" : "join timed out");
        esp_wifi_stop();
        esp_wifi_deinit();
        return start_softap();
    }

    esp_err_t Net::start_softap()
    {
        // The station interface is created and started but never asked to
        // connect: it exists so esp_wifi_scan_start has something to scan
        // with.
        want_sta_connect_ = false;

        netif_ = esp_netif_create_default_wifi_ap();
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&init));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &Net::on_wifi_event, this, nullptr));

        // Name it after the MAC so two boards on a bench are distinguishable
        // without opening either of them.
        uint8_t mac[6]{};
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

        wifi_config_t cfg{};
        const int n = std::snprintf(reinterpret_cast<char *>(cfg.ap.ssid),
                                    sizeof(cfg.ap.ssid), "pinled-%02X%02X%02X",
                                    mac[3], mac[4], mac[5]);
        cfg.ap.ssid_len = static_cast<uint8_t>(n);
        cfg.ap.channel = 1;
        cfg.ap.max_connection = 4;
        // Open, on purpose. The captive portal that follows (step 7) is how
        // someone hands over their real credentials, and putting a password on
        // this network would mean shipping one printed on the box — which is
        // not a secret, only an obstacle.
        cfg.ap.authmode = WIFI_AUTH_OPEN;

        // APSTA, not AP. esp_wifi_scan_start needs a station interface to
        // exist, and without one the provisioning page offers an empty list —
        // which is exactly how this shipped, because scanning had only ever
        // been exercised on a board already joined to a network.
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

        // Scanned once, here, before the AP has a client to disturb. The
        // result is served from cache for the life of this boot.
        cached_count_ = do_scan(cached_, kMaxCached);
        ESP_LOGI(TAG, "cached %u network(s) for the setup page",
                 (unsigned)cached_count_);

        esp_netif_ip_info_t info{};
        if (netif_ && esp_netif_get_ip_info(netif_, &info) == ESP_OK)
            std::snprintf(ip_, sizeof(ip_), IPSTR, IP2STR(&info.ip));

        mode_ = NetMode::SoftAp;
        up_ = true;

        // Only here, never in station mode: hijacking DNS on someone's home
        // network would be indefensible; on an AP we raised, for as long as
        // somebody is typing a password into it, it IS the mechanism.
        if (info.ip.addr != 0)
            dns_hijack_start(info.ip.addr);

        ESP_LOGI(TAG, "SoftAP \"%s\" (open) up — join it and any web address "
                      "will land on the setup page",
                 reinterpret_cast<char *>(cfg.ap.ssid));
        return ESP_OK;
    }

    size_t Net::scan(ApInfo *out, size_t cap)
    {
        if (out == nullptr || cap == 0)
            return 0;

        // In SoftAP mode, serve the list captured at boot rather than
        // scanning now. A scan hops channels, and the AP cannot follow — so
        // scanning on demand would disconnect the very phone that asked for
        // the list, which reads as "the setup page is broken".
        if (mode_ == NetMode::SoftAp)
        {
            const size_t n = cached_count_ < cap ? cached_count_ : cap;
            for (size_t i = 0; i < n; ++i)
                out[i] = cached_[i];
            return n;
        }

        return do_scan(out, cap);
    }

    size_t Net::do_scan(ApInfo *out, size_t cap)
    {
        if (out == nullptr || cap == 0)
            return 0;

        wifi_scan_config_t sc{};
        sc.show_hidden = false;
        if (esp_wifi_scan_start(&sc, true) != ESP_OK)
            return 0;

        uint16_t found = 0;
        esp_wifi_scan_get_ap_num(&found);
        if (found == 0)
            return 0;

        // Bounded before allocating: a crowded band can return dozens, and the
        // list is for a person to choose from.
        const uint16_t want = found > 32 ? 32 : found;
        auto *records = new (std::nothrow) wifi_ap_record_t[want];
        if (records == nullptr)
        {
            esp_wifi_scan_get_ap_records(&found, nullptr); // release the driver's copy
            return 0;
        }

        uint16_t got = want;
        esp_wifi_scan_get_ap_records(&got, records);

        // The driver already returns these strongest-first, which is the order
        // a person wants: their own network is almost always the loudest.
        //
        // De-duplicated by name, keeping the first (strongest) sighting. A
        // dual-band router or a mesh presents the same SSID several times, and
        // a dropdown offering "Pekitanoui" three identical times asks the user
        // a question they cannot answer — observed on the bench, four entries
        // for two networks.
        size_t n = 0;
        for (uint16_t i = 0; i < got && n < cap; ++i)
        {
            if (records[i].ssid[0] == '\0')
                continue; // hidden or malformed; nothing to offer

            const char *name = reinterpret_cast<const char *>(records[i].ssid);
            bool seen = false;
            for (size_t j = 0; j < n; ++j)
                if (std::strcmp(out[j].ssid, name) == 0)
                {
                    seen = true;
                    break;
                }
            if (seen)
                continue;

            std::snprintf(out[n].ssid, sizeof(out[n].ssid), "%s", name);
            out[n].rssi = records[i].rssi;
            out[n].channel = records[i].primary;
            out[n].secured = records[i].authmode != WIFI_AUTH_OPEN;
            ++n;
        }
        delete[] records;
        ESP_LOGI(TAG, "scan: %u visible, %u distinct networks", (unsigned)got, (unsigned)n);
        return n;
    }

    esp_err_t Net::start_mdns()
    {
        esp_err_t err = mdns_init();
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "mdns_init: %s — reachable by IP only", esp_err_to_name(err));
            return err;
        }
        mdns_hostname_set(hostname_);
        mdns_instance_name_set("pinled lamp controller");
        mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
        return ESP_OK;
    }
} // namespace ooe::pinled
