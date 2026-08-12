/**
 * @file api.cpp
 * @brief The device HTTP API (`/api/v1`).
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "api.h"

#include <cstdio>
#include <cstring>

#include <unistd.h> // close(), for the httpd close_fn contract
#include <memory>
#include <new>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pb_decode.h"
#include "pb_encode.h"
#include "pinled.pb.h"
#include "pinled_doc_file.h"
#include "pinled_resolve.h"

namespace ooe::pinled
{
    static const char *TAG = "api";

    namespace
    {
        constexpr char kContentType[] = "application/x-protobuf";

        /// Long enough for the response to leave the socket, short enough that
        /// a person waiting on a reboot does not wonder if it worked.
        constexpr uint64_t kRestartDelayUs = 500 * 1000;

        /// Everything a handler needs, so the handlers themselves stay free of
        /// globals and can be read in one sitting.
        struct Context
        {
            MachineConfigStore *store;
            const MachineConfig *running;
            const ChannelConfig *channels;
            size_t count;
            const Net *net;
            LiveState *live;
            httpd_handle_t server;
        };

        Context g_ctx{};

        // --- live socket -----------------------------------------------------
        //
        // Clients are tracked as raw socket descriptors because that is what
        // httpd_ws_send_frame_async takes. Four is generous for what this is:
        // one browser tab looking at a playfield, plus room for a reload that
        // has not been reaped yet.
        // Three, not four: httpd holds at most `max_open_sockets` descriptors
        // in total, and the REST endpoints need some of them. A monitor that
        // could consume every socket would make the configuration API
        // unreachable exactly when someone is trying to fix something.
        constexpr size_t kMaxLiveClients = 3;
        constexpr TickType_t kPushPeriod = pdMS_TO_TICKS(33); // ~30 Hz (FR-UI-5)

        int g_clients[kMaxLiveClients];
        size_t g_client_count = 0;
        uint32_t g_seq = 0;
        uint32_t g_dropped = 0;

        /// False when the slot table is full. The caller must then FAIL the
        /// request rather than accept it: httpd has already completed the
        /// handshake, so a silent refusal leaves the client holding an open
        /// socket that will never carry a frame — which looks exactly like a
        /// dead device.
        bool add_client(int fd)
        {
            for (size_t i = 0; i < g_client_count; ++i)
                if (g_clients[i] == fd)
                    return true;
            if (g_client_count >= kMaxLiveClients)
                return false;
            g_clients[g_client_count++] = fd;
            return true;
        }

        void drop_client(size_t index)
        {
            g_clients[index] = g_clients[--g_client_count];
        }

        /// httpd tells us when it closes a socket, which is the only reliable
        /// moment to forget one.
        ///
        /// Discovering a dead client from a failed send is too late: the OS
        /// reuses descriptors immediately, so between the close and the next
        /// push the same number can already name a DIFFERENT client — and the
        /// send that "failed" would evict the newcomer instead. The bench log
        /// showed exactly that churn, fd 58 dropping and reconnecting in the
        /// same second.
        void on_socket_closed(httpd_handle_t, int fd)
        {
            for (size_t i = 0; i < g_client_count; ++i)
            {
                if (g_clients[i] == fd)
                {
                    drop_client(i);
                    ESP_LOGI(TAG, "live: client %d closed (%u remain)",
                             fd, (unsigned)g_client_count);
                    break;
                }
            }
            close(fd); // httpd's default close_fn does exactly this
        }

        /// nanopb writes `samples` straight out of the packed buffer rather
        /// than copying it into the struct: `pinled.options` makes this field
        /// FT_CALLBACK precisely so a 256-byte array is not carried around in
        /// a message that is built thirty times a second.
        bool encode_samples(pb_ostream_t *stream, const pb_field_t *field, void *const *arg)
        {
            const auto *packed = static_cast<const uint8_t *>(*arg);
            const size_t len = *reinterpret_cast<const size_t *>(packed - sizeof(size_t));
            if (!pb_encode_tag_for_field(stream, field))
                return false;
            return pb_encode_string(stream, packed, len);
        }

        /// FR-UI-3. Applied to every response including errors — a CORS-blocked
        /// error body is indistinguishable from a network failure in a browser,
        /// which turns a clear 400 into a mystery.
        void add_cors(httpd_req_t *req)
        {
            httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
            httpd_resp_set_hdr(req, "Access-Control-Allow-Methods",
                               "GET, PUT, DELETE, OPTIONS");
            httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
            httpd_resp_set_hdr(req, "Access-Control-Max-Age", "600");
        }

        esp_err_t fail(httpd_req_t *req, httpd_err_code_t code, const char *msg)
        {
            add_cors(req);
            const char *verb = req->method == HTTP_PUT      ? "PUT"
                               : req->method == HTTP_DELETE ? "DELETE"
                               : req->method == HTTP_OPTIONS ? "OPTIONS"
                                                             : "GET";
            ESP_LOGW(TAG, "%s %s -> %s", verb, req->uri, msg);
            return httpd_resp_send_err(req, code, msg);
        }

        esp_err_t send_protobuf(httpd_req_t *req, const uint8_t *body, size_t len)
        {
            add_cors(req);
            httpd_resp_set_type(req, kContentType);
            return httpd_resp_send(req, reinterpret_cast<const char *>(body),
                                   static_cast<ssize_t>(len));
        }

        /// Preflight. Nothing but headers — see the note in api.h about why
        /// omitting this breaks browsers and not pbtool.
        esp_err_t handle_options(httpd_req_t *req)
        {
            add_cors(req);
            httpd_resp_set_status(req, "204 No Content");
            return httpd_resp_send(req, nullptr, 0);
        }

        void restart_soon()
        {
            esp_timer_create_args_t args{};
            args.callback = [](void *) {
                ESP_LOGW(TAG, "restarting to apply configuration (FR-CFG-16)");
                esp_restart();
            };
            args.name = "apply-restart";
            esp_timer_handle_t t = nullptr;
            if (esp_timer_create(&args, &t) == ESP_OK)
                esp_timer_start_once(t, kRestartDelayUs);
        }

        // ------------------------------------------------------------ GET --

        /// Stream a stored document's payload without decoding it.
        /// Returns false when there is nothing stored; any other failure has
        /// already been answered.
        bool send_stored(httpd_req_t *req, const char *path)
        {
            const size_t cap = doc_frame_size(pinled_v1_MachineProfile_size);
            std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[cap]);
            if (!buf)
            {
                fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
                return true;
            }

            const uint8_t *payload = nullptr;
            size_t len = 0;
            DocStatus ds = DocStatus::Ok;
            const FileStatus fs = doc_file_read(path, buf.get(), cap,
                                                nullptr, &payload, &len, &ds);

            if (fs == FileStatus::NotFound)
                return false;

            if (fs != FileStatus::Ok)
            {
                // A stored document that will not read is reported, not hidden
                // behind a synthesised one — the UI needs to be able to say
                // "this device has a corrupt configuration and is running
                // defaults", which is a different situation from "unconfigured".
                ESP_LOGE(TAG, "%s unreadable: %s (%s)", path,
                         file_status_str(fs), doc_status_str(ds));
                fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, doc_status_str(ds));
                return true;
            }

            send_protobuf(req, payload, len);
            return true;
        }

        esp_err_t get_config(httpd_req_t *req)
        {
            if (send_stored(req, kInstallPath))
                return ESP_OK;

            // Nothing stored: describe what is actually running, so a fresh
            // device is not a blank page. See machine_to_install()'s note on
            // why this is only ever the fallback.
            std::unique_ptr<pinled_v1_InstallConfig> doc(
                new (std::nothrow) pinled_v1_InstallConfig());
            const size_t cap = pinled_v1_InstallConfig_size;
            std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[cap]);
            if (!doc || !buf)
                return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");

            machine_to_install(*g_ctx.running, g_ctx.channels, g_ctx.count, *doc);

            pb_ostream_t os = pb_ostream_from_buffer(buf.get(), cap);
            if (!pb_encode(&os, pinled_v1_InstallConfig_fields, doc.get()))
                return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "encode failed");

            return send_protobuf(req, buf.get(), os.bytes_written);
        }

        esp_err_t get_profile(httpd_req_t *req)
        {
            if (send_stored(req, kProfilePath))
                return ESP_OK;

            // An unconfigured device has no styled lamps. An empty profile is
            // the honest answer; synthesising entries for every channel would
            // claim someone had chosen them.
            return send_protobuf(req, nullptr, 0);
        }

        esp_err_t get_info(httpd_req_t *req)
        {
            pinled_v1_DeviceInfo info = pinled_v1_DeviceInfo_init_zero;
            info.api_version = kApiVersion;

            // IDF's version field is 32 bytes and the schema's is 16
            // (pinled.options), so this can genuinely truncate. Bounded
            // explicitly rather than silenced: 16 is ample for semver, and a
            // build that adopts long version strings should widen the schema
            // field on purpose rather than discover it in a UI.
            const esp_app_desc_t *app = esp_app_get_description();
            std::snprintf(info.firmware_version, sizeof(info.firmware_version),
                          "%.*s", static_cast<int>(sizeof(info.firmware_version)) - 1,
                          app ? app->version : "unknown");

            // FR-REG-1: identity is the factory MAC. No provisioning step, no
            // stored secret, nothing to leak or reset.
            uint8_t mac[6]{};
            esp_efuse_mac_get_default(mac);
            std::snprintf(info.device_id, sizeof(info.device_id),
                          "%02X%02X%02X%02X%02X%02X",
                          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

            info.has_geometry = true;
            info.geometry.num_modules = static_cast<uint32_t>(g_ctx.running->num_modules);
            info.geometry.channels_per_module =
                static_cast<uint32_t>(g_ctx.running->channels_per_module);
            info.geometry.led_count = static_cast<uint32_t>(g_ctx.running->led_count);

            // OTA is not in this step; reporting it plainly as unarmed is
            // better than omitting the field and leaving the UI to guess.
            info.ota_armed = false;
            info.ota_arm_seconds_left = 0;

            uint8_t buf[pinled_v1_DeviceInfo_size];
            pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
            if (!pb_encode(&os, pinled_v1_DeviceInfo_fields, &info))
                return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "encode failed");

            return send_protobuf(req, buf, os.bytes_written);
        }

        // ------------------------------------------------------------ PUT --

        esp_err_t put_document(httpd_req_t *req, DocKind kind, const pb_msgdesc_t *fields)
        {
            const size_t len = static_cast<size_t>(req->content_len);
            if (len > kMaxDocBytes - kDocHeaderSize)
                return fail(req, HTTPD_400_BAD_REQUEST, "document too large");

            std::unique_ptr<uint8_t[]> body(new (std::nothrow) uint8_t[len ? len : 1]);
            if (!body)
                return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");

            // A stalled client must not own the server. httpd runs one handler
            // at a time, so an unbounded `continue` on timeout is not a slow
            // request — it is the whole API hung by anyone who opens a PUT,
            // declares a length and then says nothing.
            constexpr int kMaxStalls = 4;
            int stalls = 0;
            size_t got = 0;
            while (got < len)
            {
                const int n = httpd_req_recv(req, reinterpret_cast<char *>(body.get()) + got,
                                             len - got);
                if (n == HTTPD_SOCK_ERR_TIMEOUT)
                {
                    if (++stalls > kMaxStalls)
                        return fail(req, HTTPD_408_REQ_TIMEOUT, "body never arrived");
                    continue;
                }
                if (n <= 0)
                    return fail(req, HTTPD_400_BAD_REQUEST, "truncated body");
                stalls = 0;
                got += static_cast<size_t>(n);
            }

            // Decoded ONLY to validate, and the result is thrown away. The
            // bytes that get stored are the bytes that arrived, so fields this
            // firmware cannot represent survive being written and read back
            // (FR-UI-4). Storing a re-encode would quietly narrow every
            // document to what this build happens to understand.
            // Both decode targets are heap, and each is the type actually
            // being decoded. The first version allocated a MachineProfile
            // whatever the endpoint was and put an InstallConfig on the stack
            // — 1.6 KB of it, inside a 6 KB httpd task, to save an allocation
            // it was making anyway.
            if (fields == pinled_v1_InstallConfig_fields)
            {
                std::unique_ptr<pinled_v1_InstallConfig> doc(
                    new (std::nothrow) pinled_v1_InstallConfig());
                if (!doc)
                    return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
                *doc = pinled_v1_InstallConfig_init_zero;

                pb_istream_t is = pb_istream_from_buffer(body.get(), got);
                if (!pb_decode(&is, fields, doc.get()))
                    return fail(req, HTTPD_400_BAD_REQUEST, "not a valid document");

                // An install config that cannot be projected would leave the
                // device unbootable-as-configured after the restart. Rejecting
                // it here costs the caller a 400; accepting it costs a trip to
                // the bench with a USB cable.
                MachineConfig probe{};
                const InstallStatus ok =
                    install_to_machine(*doc, probe, MachineConfigStore::defaults());
                if (ok != InstallStatus::Ok)
                    return fail(req, HTTPD_400_BAD_REQUEST, install_status_str(ok));
            }
            else
            {
                std::unique_ptr<pinled_v1_MachineProfile> doc(
                    new (std::nothrow) pinled_v1_MachineProfile());
                if (!doc)
                    return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
                *doc = pinled_v1_MachineProfile_init_zero;

                pb_istream_t is = pb_istream_from_buffer(body.get(), got);
                if (!pb_decode(&is, fields, doc.get()))
                    return fail(req, HTTPD_400_BAD_REQUEST, "not a valid document");
            }

            if (g_ctx.store->save_document(kind, body.get(), got) != ESP_OK)
                return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");

            pinled_v1_ApplyResult result = pinled_v1_ApplyResult_init_zero;
            result.accepted = true;
            result.restarted = true;
            std::snprintf(result.message, sizeof(result.message),
                          "stored %u bytes; restarting", (unsigned)got);

            uint8_t out[pinled_v1_ApplyResult_size];
            pb_ostream_t os = pb_ostream_from_buffer(out, sizeof(out));
            pb_encode(&os, pinled_v1_ApplyResult_fields, &result);

            const esp_err_t err = send_protobuf(req, out, os.bytes_written);
            restart_soon();
            return err;
        }

        esp_err_t put_config(httpd_req_t *req)
        {
            return put_document(req, DocKind::InstallConfig, pinled_v1_InstallConfig_fields);
        }

        esp_err_t put_profile(httpd_req_t *req)
        {
            return put_document(req, DocKind::MachineProfile, pinled_v1_MachineProfile_fields);
        }

        // --------------------------------------------------------- DELETE --

        esp_err_t delete_document(httpd_req_t *req, DocKind kind)
        {
            if (g_ctx.store->erase_document(kind) != ESP_OK)
                return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "erase failed");

            pinled_v1_ApplyResult result = pinled_v1_ApplyResult_init_zero;
            result.accepted = true;
            result.restarted = true;
            std::snprintf(result.message, sizeof(result.message),
                          "erased; restarting to build defaults");

            uint8_t out[pinled_v1_ApplyResult_size];
            pb_ostream_t os = pb_ostream_from_buffer(out, sizeof(out));
            pb_encode(&os, pinled_v1_ApplyResult_fields, &result);

            const esp_err_t err = send_protobuf(req, out, os.bytes_written);
            restart_soon();
            return err;
        }

        esp_err_t delete_config(httpd_req_t *req)
        {
            return delete_document(req, DocKind::InstallConfig);
        }
        esp_err_t delete_profile(httpd_req_t *req)
        {
            return delete_document(req, DocKind::MachineProfile);
        }
        /// The upgrade handshake. httpd performs it for us when the handler is
        /// registered as a websocket; this only runs for the GET that opens it
        /// and for subsequent frames from the client, which are ignored — the
        /// stream is one-way by design (FR-UI-5).
        esp_err_t live_handler(httpd_req_t *req)
        {
            if (req->method == HTTP_GET)
            {
                const int fd = httpd_req_to_sockfd(req);
                if (!add_client(fd))
                {
                    ESP_LOGW(TAG, "live: refusing client %d, %u already connected",
                             fd, (unsigned)g_client_count);
                    return ESP_FAIL; // closes the socket; silence would be a lie
                }
                ESP_LOGI(TAG, "live: client %d connected (%u total)",
                         fd, (unsigned)g_client_count);
                return ESP_OK;
            }

            // Drain and discard whatever the client sent. Not answering at all
            // would leave the frame in the socket buffer and stall the next
            // read; there is simply nothing a client can usefully say here yet.
            httpd_ws_frame_t frame{};
            frame.type = HTTPD_WS_TYPE_BINARY;
            return httpd_ws_recv_frame(req, &frame, 0);
        }

        void push_task(void *)
        {
            // Sized once, off the task stack: the packed payload plus the
            // protobuf envelope around it.
            const size_t packed_cap =
                LiveState::kMaxChannels * kLiveBytesPerChannel + sizeof(size_t);
            auto *packed_alloc = new (std::nothrow) uint8_t[packed_cap];
            auto *frame_buf = new (std::nothrow) uint8_t[packed_cap + 32];
            if (!packed_alloc || !frame_buf)
            {
                ESP_LOGE(TAG, "live: no memory for the push task; not starting");
                delete[] packed_alloc;
                delete[] frame_buf;
                vTaskDelete(nullptr);
                return;
            }
            // The callback needs the length alongside the bytes, and nanopb
            // gives it one void*. Stashing the length immediately before the
            // payload keeps that to one pointer.
            uint8_t *packed = packed_alloc + sizeof(size_t);

            // vTaskDelayUntil, not vTaskDelay: the latter sleeps for the period
            // AFTER the work, so the real rate is 1/(work + period). Measured
            // on the bench that was 21.4 Hz against a nominal 30 — the encode
            // and send cost ~14 ms at this priority. A fixed wake-up absorbs
            // that instead of adding to it.
            TickType_t next = xTaskGetTickCount();

            for (;;)
            {
                vTaskDelayUntil(&next, kPushPeriod);

                if (g_client_count == 0)
                    continue;

                const size_t len = g_ctx.live->snapshot(packed, packed_cap - sizeof(size_t));
                if (len == 0)
                    continue;
                *reinterpret_cast<size_t *>(packed_alloc) = len;

                pinled_v1_LiveFrame lf = pinled_v1_LiveFrame_init_zero;
                lf.seq = ++g_seq;
                lf.channel_count = static_cast<uint32_t>(g_ctx.live->count);
                lf.samples.funcs.encode = &encode_samples;
                lf.samples.arg = packed;

                pb_ostream_t os = pb_ostream_from_buffer(frame_buf, packed_cap + 32);
                if (!pb_encode(&os, pinled_v1_LiveFrame_fields, &lf))
                    continue;

                httpd_ws_frame_t ws{};
                ws.type = HTTPD_WS_TYPE_BINARY;
                ws.payload = frame_buf;
                ws.len = os.bytes_written;

                for (size_t i = 0; i < g_client_count;)
                {
                    const esp_err_t err =
                        httpd_ws_send_frame_async(g_ctx.server, g_clients[i], &ws);
                    if (err == ESP_OK)
                    {
                        ++i;
                        continue;
                    }
                    // A client that will not take a frame is dropped, not
                    // waited for. FR-UI-9: the monitor never becomes a reason
                    // the scan is late, and the sticky bit means the next push
                    // carries whatever this one could not.
                    // Counts CLIENTS reaped, not pushes lost — the sticky bit
                    // means no push is ever lost, only deferred, and a log line
                    // claiming otherwise would send someone hunting a data-loss
                    // bug that cannot exist.
                    ++g_dropped;
                    ESP_LOGW(TAG, "live: client %d dropped (%s), %u dropped since boot",
                             g_clients[i], esp_err_to_name(err), (unsigned)g_dropped);
                    drop_client(i);
                }
            }
        }
    } // namespace

    esp_err_t Api::start(MachineConfigStore &store,
                         const MachineConfig &running,
                         const ChannelConfig *channels, size_t count,
                         const Net &net,
                         LiveState &live)
    {
        g_ctx = {&store, &running, channels, count, &net, &live, nullptr};

        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        // Nine handlers below, and the default cap is eight — which fails at
        // registration time with a log line nobody reads.
        cfg.max_uri_handlers = 16;
        cfg.max_open_sockets = 7;
        // LRU purging closes the least recently used socket when a new one
        // arrives — and a live WebSocket looks idle to httpd, because the
        // traffic is all outbound. With purging on, opening a second browser
        // tab would silently kill the first one's monitor. Dead sockets are
        // reaped by close_fn and by the send-failure path instead.
        cfg.lru_purge_enable = false;
        cfg.close_fn = on_socket_closed;
        cfg.stack_size = 6144; // protobuf encode plus TLS-free httpd

        httpd_handle_t server = nullptr;
        const esp_err_t err = httpd_start(&server, &cfg);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
            return err;
        }
        server_ = server;
        g_ctx.server = server;

        // Fields are assigned rather than brace-initialised: httpd_uri_t gains
        // and loses trailing members with CONFIG_HTTPD_WS_SUPPORT, so a
        // positional initialiser here compiles or does not depending on a
        // Kconfig option in another component. Step 6 turns that option on.
        struct Route
        {
            const char *uri;
            httpd_method_t method;
            esp_err_t (*handler)(httpd_req_t *);
        };
        static const Route routes[] = {
            {"/api/v1/info", HTTP_GET, get_info},
            {"/api/v1/config", HTTP_GET, get_config},
            {"/api/v1/config", HTTP_PUT, put_config},
            {"/api/v1/config", HTTP_DELETE, delete_config},
            {"/api/v1/config", HTTP_OPTIONS, handle_options},
            {"/api/v1/profile", HTTP_GET, get_profile},
            {"/api/v1/profile", HTTP_PUT, put_profile},
            {"/api/v1/profile", HTTP_DELETE, delete_profile},
            {"/api/v1/profile", HTTP_OPTIONS, handle_options},
        };
        for (const auto &r : routes)
        {
            httpd_uri_t u{};
            u.uri = r.uri;
            u.method = r.method;
            u.handler = r.handler;
            u.user_ctx = nullptr;
            ESP_ERROR_CHECK(httpd_register_uri_handler(server, &u));
        }

        // Registered separately because it is the one route that is a
        // websocket, and is_websocket is the field whose presence depends on
        // CONFIG_HTTPD_WS_SUPPORT.
        {
            httpd_uri_t ws{};
            ws.uri = "/api/v1/live";
            ws.method = HTTP_GET;
            ws.handler = live_handler;
            ws.user_ctx = nullptr;
            ws.is_websocket = true;
            ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ws));
        }

        // Priority 3 and core 0: below the render task, far below the scan,
        // and off the core the scan owns. The monitor must never be why a
        // frame is late (FR-UI-9).
        if (xTaskCreatePinnedToCore(push_task, "pinled_live", 4096, nullptr,
                                    3, nullptr, 0) != pdPASS)
            ESP_LOGE(TAG, "live: push task did not start");

        ESP_LOGI(TAG, "API v%u on http://%s.local/api/v1/ (also http://%s/api/v1/)",
                 (unsigned)kApiVersion, net.hostname(), net.ip());
        return ESP_OK;
    }

    void Api::stop()
    {
        if (server_)
        {
            httpd_stop(static_cast<httpd_handle_t>(server_));
            server_ = nullptr;
        }
    }
} // namespace ooe::pinled
