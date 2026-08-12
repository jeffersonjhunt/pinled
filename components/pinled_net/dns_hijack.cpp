/**
 * @file dns_hijack.cpp
 * @brief Answer every DNS query with our own address (captive portal).
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * @par What this is for
 * Phones and laptops decide whether a network "has internet" by fetching a
 * known URL the moment they associate. Answering every name with the device's
 * own address is what turns that probe into a hit on our page, which is what
 * makes the OS pop the sign-in sheet automatically. Without it the user has to
 * be told an IP address, which is precisely the instruction FR-UI-6 exists to
 * avoid.
 *
 * @par Deliberately minimal, and deliberately dumb
 * This is not a DNS server. It parses just enough of a query to echo it back
 * with one A record appended, ignores every question type it does not
 * understand, and never allocates. Anything cleverer would be a second network
 * service to get wrong on a board whose purpose is lighting lamps.
 *
 * It runs **only in SoftAP mode**. Hijacking DNS on someone's home network
 * would be indefensible; on an AP we raised, for the ninety seconds someone is
 * typing a password into it, it is the entire mechanism.
 */

#include "dns_hijack.h"

#include <cstring>
#include <new>

#include <lwip/sockets.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace ooe::pinled
{
    namespace
    {
        const char *TAG = "dns";

        constexpr uint16_t kDnsPort = 53;
        constexpr size_t kMaxPacket = 512; // the classic UDP DNS limit
        constexpr uint32_t kTtlSeconds = 30;

        struct __attribute__((packed)) DnsHeader
        {
            uint16_t id;
            uint16_t flags;
            uint16_t questions;
            uint16_t answers;
            uint16_t authority;
            uint16_t additional;
        };

        /// Walk a QNAME's length-prefixed labels to find where it ends.
        /// Returns 0 if the name runs past the end of the packet.
        size_t skip_qname(const uint8_t *p, size_t len, size_t at)
        {
            while (at < len && p[at] != 0)
            {
                // Compression pointers cannot appear in a question section from
                // a sane resolver, and following one would mean parsing
                // arbitrary offsets. Refuse rather than chase it.
                if ((p[at] & 0xC0) != 0)
                    return 0;
                at += static_cast<size_t>(p[at]) + 1;
            }
            return (at < len) ? at + 1 : 0;
        }

        void dns_task(void *arg)
        {
            const uint32_t our_ip = *static_cast<uint32_t *>(arg);
            delete static_cast<uint32_t *>(arg);

            const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
            if (sock < 0)
            {
                ESP_LOGE(TAG, "socket: errno %d", errno);
                vTaskDelete(nullptr);
                return;
            }

            sockaddr_in bind_addr{};
            bind_addr.sin_family = AF_INET;
            bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
            bind_addr.sin_port = htons(kDnsPort);
            if (bind(sock, reinterpret_cast<sockaddr *>(&bind_addr), sizeof(bind_addr)) < 0)
            {
                ESP_LOGE(TAG, "bind: errno %d", errno);
                close(sock);
                vTaskDelete(nullptr);
                return;
            }

            ESP_LOGI(TAG, "captive portal DNS up; every name resolves here");

            uint8_t buf[kMaxPacket];
            for (;;)
            {
                sockaddr_in from{};
                socklen_t from_len = sizeof(from);
                const int n = recvfrom(sock, buf, sizeof(buf), 0,
                                       reinterpret_cast<sockaddr *>(&from), &from_len);
                if (n < static_cast<int>(sizeof(DnsHeader)))
                    continue;

                auto *hdr = reinterpret_cast<DnsHeader *>(buf);
                if ((ntohs(hdr->flags) & 0x8000) != 0)
                    continue; // a response, not a query
                if (ntohs(hdr->questions) != 1)
                    continue; // one question is what every client actually sends

                const size_t qname_end = skip_qname(buf, static_cast<size_t>(n),
                                                    sizeof(DnsHeader));
                if (qname_end == 0 || qname_end + 4 > static_cast<size_t>(n))
                    continue;

                uint16_t qtype = 0;
                std::memcpy(&qtype, buf + qname_end, sizeof(qtype));
                if (ntohs(qtype) != 1) // A records only
                    continue;

                const size_t question_end = qname_end + 4;
                const size_t answer_len = 2 + 2 + 2 + 4 + 2 + 4;
                if (question_end + answer_len > sizeof(buf))
                    continue;

                hdr->flags = htons(0x8180); // response, recursion available
                hdr->answers = htons(1);

                uint8_t *a = buf + question_end;
                a[0] = 0xC0; // pointer to the question's name at offset 12
                a[1] = 0x0C;
                a[2] = 0x00; a[3] = 0x01; // type A
                a[4] = 0x00; a[5] = 0x01; // class IN
                const uint32_t ttl = htonl(kTtlSeconds);
                std::memcpy(a + 6, &ttl, 4);
                a[10] = 0x00; a[11] = 0x04; // rdlength
                std::memcpy(a + 12, &our_ip, 4);

                sendto(sock, buf, question_end + answer_len, 0,
                       reinterpret_cast<sockaddr *>(&from), from_len);
            }
        }
    } // namespace

    esp_err_t dns_hijack_start(uint32_t ip_be)
    {
        auto *ip = new (std::nothrow) uint32_t(ip_be);
        if (ip == nullptr)
            return ESP_ERR_NO_MEM;

        // Low priority and a small stack: it answers a handful of packets while
        // somebody types a password, and must never compete with the scan.
        if (xTaskCreatePinnedToCore(dns_task, "pinled_dns", 3072, ip, 3, nullptr, 0) != pdPASS)
        {
            delete ip;
            return ESP_FAIL;
        }
        return ESP_OK;
    }
} // namespace ooe::pinled
