/**
 * @file net.cpp
 * @brief Wi-Fi station or SoftAP, plus mDNS.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "net.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
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

        bool have_credentials()
        {
            return sizeof(CONFIG_PINLED_WIFI_SSID) > 1;
        }
    } // namespace

    void Net::on_wifi_event(void *arg, esp_event_base_t, int32_t id, void *)
    {
        Net *self = static_cast<Net *>(arg);
        auto group = static_cast<EventGroupHandle_t>(self->connected_);

        switch (id)
        {
        case WIFI_EVENT_STA_START:
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

        const esp_err_t err = have_credentials() ? start_station() : start_softap();
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

    esp_err_t Net::start_station()
    {
        ESP_LOGW(TAG, "station mode with BUILD-TIME credentials — bring-up only, "
                      "replaced by provisioning at step 7 (FR-CFG-12)");

        auto group = xEventGroupCreate();
        connected_ = group;
        netif_ = esp_netif_create_default_wifi_sta();

        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&init));

        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &Net::on_wifi_event, this, nullptr));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &Net::on_ip_event, this, nullptr));

        wifi_config_t cfg{};
        std::snprintf(reinterpret_cast<char *>(cfg.sta.ssid), sizeof(cfg.sta.ssid),
                      "%s", CONFIG_PINLED_WIFI_SSID);
        std::snprintf(reinterpret_cast<char *>(cfg.sta.password), sizeof(cfg.sta.password),
                      "%s", CONFIG_PINLED_WIFI_PASSWORD);

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
        ESP_LOGI(TAG, "joining \"%s\"...", CONFIG_PINLED_WIFI_SSID);

        const EventBits_t bits = xEventGroupWaitBits(
            group, kConnectedBit | kFailedBit, pdFALSE, pdFALSE, kJoinTimeout);

        if (bits & kConnectedBit)
        {
            mode_ = NetMode::Station;
            up_ = true;
            return ESP_OK;
        }

        // Deliberately not an error return. A board that cannot reach the
        // configured network must still finish booting and still run the scan
        // and render path; the lamps working is not contingent on Wi-Fi.
        ESP_LOGE(TAG, "%s — the API will not be reachable",
                 (bits & kFailedBit) ? "join failed" : "join timed out");
        mode_ = NetMode::Station;
        up_ = false;
        return ESP_OK;
    }

    esp_err_t Net::start_softap()
    {
        netif_ = esp_netif_create_default_wifi_ap();

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

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
        ESP_ERROR_CHECK(esp_wifi_start());

        esp_netif_ip_info_t info{};
        if (netif_ && esp_netif_get_ip_info(netif_, &info) == ESP_OK)
            std::snprintf(ip_, sizeof(ip_), IPSTR, IP2STR(&info.ip));

        mode_ = NetMode::SoftAp;
        up_ = true;
        ESP_LOGI(TAG, "SoftAP \"%s\" (open) up", reinterpret_cast<char *>(cfg.ap.ssid));
        return ESP_OK;
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
