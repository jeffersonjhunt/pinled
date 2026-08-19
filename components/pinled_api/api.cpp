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
#include "credentials.h"
#include "pinled_resolve.h"
#include "ota_manager.h"
#include "esp_ota_ops.h" // for ESP_ERR_OTA_VALIDATE_FAILED in post_ota

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
            Net *net_mutable; ///< scanning is not a const operation
            LiveState *live;
            ProfilerControl *profiler; ///< null on a build with no scan task
            OtaManager *ota;           ///< null on a build with no OTA slots
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
            // POST is listed for the reader's benefit rather than the
            // browser's: it is a CORS-safelisted method, so a preflight
            // accepts it whether or not it appears here. Leaving it out was
            // correct and looked like a bug every time anyone checked.
            httpd_resp_set_hdr(req, "Access-Control-Allow-Methods",
                               "GET, POST, PUT, DELETE, OPTIONS");
            httpd_resp_set_hdr(req, "Access-Control-Allow-Headers",
                               "Content-Type, X-Image-SHA256");
            httpd_resp_set_hdr(req, "Access-Control-Max-Age", "600");
        }

        esp_err_t fail(httpd_req_t *req, httpd_err_code_t code, const char *msg)
        {
            add_cors(req);
            const char *verb = req->method == HTTP_PUT       ? "PUT"
                               : req->method == HTTP_POST    ? "POST"
                               : req->method == HTTP_DELETE  ? "DELETE"
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

        // --- captive portal ------------------------------------------------
        //
        // Self-contained on the device (FR-UI-6): no CDN, no font, no
        // framework, nothing fetched. A setup page that needs the internet to
        // tell a device about the internet is a page that does not work.
        //
        // It is also deliberately plain. This is seen once per device, on a
        // phone, by someone standing next to a pinball machine — the job is a
        // list, a box and a button.
        constexpr char kPortalPage[] =
            "<!doctype html><meta charset=utf-8>"
            "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
            "<title>pinled setup</title>"
            "<style>"
            "body{font:16px system-ui,sans-serif;margin:0;padding:24px;"
            "background:#12100D;color:#EDE6DA}"
            "h1{font-size:20px;margin:0 0 4px}p{color:#9A9086;margin:0 0 20px}"
            "label{display:block;margin:16px 0 6px;font-size:13px;color:#9A9086}"
            "select,input,button{width:100%;box-sizing:border-box;font-size:16px;"
            "padding:12px;border-radius:8px;border:1px solid #3A342C;"
            "background:#1C1915;color:#EDE6DA}"
            "button{margin-top:24px;background:#FFB23F;color:#12100D;border:0;"
            "font-weight:600}"
            "#s{margin-top:16px;color:#9A9086;font-size:14px}"
            "</style>"
            "<h1>pinled</h1><p>Choose the network this controller should join.</p>"
            "<label for=n>Network</label><select id=n></select>"
            // Always present, never conditional on the scan. A hidden network
            // appears in no scan at all, and a scan that finds nothing must
            // still leave a way forward — the first version offered a dropdown
            // and nothing else, so an empty list was a dead end.
            "<label for=m>...or type the name</label>"
            "<input id=m autocomplete=off autocapitalize=none spellcheck=false "
            "placeholder=\"for a hidden network\">"
            "<label for=p>Password</label>"
            "<input id=p type=password autocomplete=current-password placeholder=\"leave empty if open\">"
            "<button onclick=save()>Join</button><div id=s></div>"
            "<script>"
            "var S=document.getElementById('s');"
            // Protobuf by hand, because the whole schema in a page served off
            // a microcontroller would be absurd for two strings. Field 1 and
            // field 2, both length-delimited.
            "function f(n,v){var b=new TextEncoder().encode(v);"
            "var o=[n<<3|2];var l=b.length;while(l>127){o.push(l&127|128);l>>=7}o.push(l);"
            "return o.concat(Array.from(b))}"
            "fetch('/api/v1/scan').then(r=>r.arrayBuffer()).then(b=>{"
            "var d=new Uint8Array(b),i=0,o=document.getElementById('n');"
            "while(i<d.length){var k=d[i++];if((k>>3)!=1||(k&7)!=2)break;"
            "var n=d[i++],e=i+n,ss='';"
            "while(i<e){var k2=d[i++],t=k2>>3,w=k2&7;"
            "if(w==2){var m=d[i++];if(t==1)ss=new TextDecoder().decode(d.slice(i,i+m));i+=m}"
            "else if(w==0){while(d[i]&128)i++;i++}else break}"
            "if(ss){var op=document.createElement('option');op.textContent=ss;o.appendChild(op)}}"
            "if(!o.options.length){"
            "var op=document.createElement('option');op.textContent='(none found)';"
            "op.value='';o.appendChild(op);"
            "S.textContent='No networks found — type the name below.'}"
            "}).catch(e=>{S.textContent='Scan unavailable — type the name below.'});"
            "function save(){"
            // The typed name wins when present: someone who filled it in meant
            // it, and the dropdown may be showing a placeholder.
            "var ss=document.getElementById('m').value.trim()"
            "||document.getElementById('n').value,"
            "pw=document.getElementById('p').value;"
            "if(!ss){S.textContent='Pick a network or type its name.';return}"
            "S.textContent='Joining '+ss+'...';"
            "var body=new Uint8Array(f(1,ss).concat(pw?f(2,pw):[]));"
            "fetch('/api/v1/provision',{method:'POST',"
            "headers:{'Content-Type':'application/x-protobuf'},body:body})"
            ".then(r=>{S.textContent=r.ok?"
            "'Accepted. The controller is restarting and will join '+ss+'. "
            "This network will disappear.':'Rejected ('+r.status+')'})"
            ".catch(e=>{S.textContent='Sent; the controller is restarting.'})}"
            "</script>";

        esp_err_t get_portal(httpd_req_t *req)
        {
            add_cors(req);
            httpd_resp_set_type(req, "text/html");
            return httpd_resp_send(req, kPortalPage, HTTPD_RESP_USE_STRLEN);
        }

        /// Every OS probes a different URL to decide whether a network is
        /// captive. Redirecting them all is what makes the phone pop the page
        /// by itself instead of the user being told to type an IP address.
        esp_err_t redirect_to_portal(httpd_req_t *req)
        {
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "/");
            return httpd_resp_send(req, nullptr, 0);
        }

        esp_err_t get_scan(httpd_req_t *req)
        {
            auto *aps = new (std::nothrow) ApInfo[20];
            if (aps == nullptr)
                return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");

            const size_t n = g_ctx.net_mutable->scan(aps, 20);

            pinled_v1_ScanResult result = pinled_v1_ScanResult_init_zero;
            result.aps_count = static_cast<pb_size_t>(n);
            for (size_t i = 0; i < n; ++i)
            {
                std::snprintf(result.aps[i].ssid, sizeof(result.aps[i].ssid), "%s", aps[i].ssid);
                result.aps[i].rssi = aps[i].rssi;
                result.aps[i].channel = aps[i].channel;
                result.aps[i].secured = aps[i].secured;
            }
            delete[] aps;

            uint8_t buf[pinled_v1_ScanResult_size];
            pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
            if (!pb_encode(&os, pinled_v1_ScanResult_fields, &result))
                return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "encode failed");
            return send_protobuf(req, buf, os.bytes_written);
        }

        esp_err_t post_provision(httpd_req_t *req)
        {
            const size_t len = static_cast<size_t>(req->content_len);
            if (len == 0 || len > 256)
                return fail(req, HTTPD_400_BAD_REQUEST, "implausible credential body");

            uint8_t body[256];
            size_t got = 0;
            int stalls = 0;
            while (got < len)
            {
                const int n = httpd_req_recv(req, reinterpret_cast<char *>(body) + got,
                                             len - got);
                if (n == HTTPD_SOCK_ERR_TIMEOUT)
                {
                    if (++stalls > 4)
                        return fail(req, HTTPD_408_REQ_TIMEOUT, "body never arrived");
                    continue;
                }
                if (n <= 0)
                    return fail(req, HTTPD_400_BAD_REQUEST, "truncated body");
                stalls = 0;
                got += static_cast<size_t>(n);
            }

            pinled_v1_WifiCredentials msg = pinled_v1_WifiCredentials_init_zero;
            pb_istream_t is = pb_istream_from_buffer(body, got);
            if (!pb_decode(&is, pinled_v1_WifiCredentials_fields, &msg))
                return fail(req, HTTPD_400_BAD_REQUEST, "not valid credentials");
            if (msg.ssid[0] == '\0')
                return fail(req, HTTPD_400_BAD_REQUEST, "empty ssid");

            WifiCredentials creds{};
            std::snprintf(creds.ssid, sizeof(creds.ssid), "%s", msg.ssid);
            std::snprintf(creds.password, sizeof(creds.password), "%s", msg.password);

            // Wiped from the request buffer immediately. It still exists in
            // NVS and in the radio's config, so this is hygiene rather than a
            // guarantee — but a passphrase has no business lingering in a heap
            // block that the next request will reuse.
            std::memset(body, 0, sizeof(body));
            std::memset(&msg, 0, sizeof(msg));

            if (credentials_save(creds) != ESP_OK)
                return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not store");

            pinled_v1_ApplyResult result = pinled_v1_ApplyResult_init_zero;
            result.accepted = true;
            result.restarted = true;
            std::snprintf(result.message, sizeof(result.message),
                          "provisioned; restarting to join");

            uint8_t out[pinled_v1_ApplyResult_size];
            pb_ostream_t os = pb_ostream_from_buffer(out, sizeof(out));
            pb_encode(&os, pinled_v1_ApplyResult_fields, &result);

            const esp_err_t err = send_protobuf(req, out, os.bytes_written);
            restart_soon();
            return err;
        }

        /// Forget the network and restart into the SoftAP — the same outcome
        /// as the button hold (FR-UI-7), reachable from the UI. Someone
        /// moving a machine to a new building should not have to open the
        /// cabinet to do it.
        esp_err_t delete_provision(httpd_req_t *req)
        {
            if (credentials_erase() != ESP_OK)
                return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not erase");

            pinled_v1_ApplyResult result = pinled_v1_ApplyResult_init_zero;
            result.accepted = true;
            result.restarted = true;
            std::snprintf(result.message, sizeof(result.message),
                          "network forgotten; restarting into setup mode");

            uint8_t out[pinled_v1_ApplyResult_size];
            pb_ostream_t os = pb_ostream_from_buffer(out, sizeof(out));
            pb_encode(&os, pinled_v1_ApplyResult_fields, &result);

            const esp_err_t err = send_protobuf(req, out, os.bytes_written);
            restart_soon();
            return err;
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

            info.ota_pending = false;
            info.ota_confirm_seconds_left = 0;
            if (g_ctx.ota != nullptr && g_ctx.ota->phase() == OtaPhase::PENDING)
            {
                info.ota_pending = true;
                // Rounded UP: an image with 1 ms left is still confirmable,
                // and a countdown that reads 0 while the button still works
                // would teach people the number lies.
                info.ota_confirm_seconds_left =
                    (g_ctx.ota->remaining_ms() + 999) / 1000;
            }

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

        // ------------------------------------------------------- profiler --
        //
        // Note this is the CLASSIFIER, not the stored MachineProfile document
        // one route above. Nothing here reads or writes flash.

        /// The runtime enum and the wire enum are deliberately separate types:
        /// the wire one has UNSPECIFIED at zero as proto3 requires, and the
        /// runtime one has IDLE there because a zeroed struct describes a
        /// device that is doing nothing. Translating explicitly is what keeps
        /// that difference from becoming an off-by-one nobody notices.
        pinled_v1_ProfilerState wire_state(ProfilerState s)
        {
            switch (s)
            {
            case ProfilerState::IDLE:
                return pinled_v1_ProfilerState_PROFILER_STATE_IDLE;
            case ProfilerState::OBSERVING:
                return pinled_v1_ProfilerState_PROFILER_STATE_OBSERVING;
            case ProfilerState::CLASSIFYING:
                return pinled_v1_ProfilerState_PROFILER_STATE_CLASSIFYING;
            case ProfilerState::UNAVAILABLE:
                return pinled_v1_ProfilerState_PROFILER_STATE_UNAVAILABLE;
            }
            return pinled_v1_ProfilerState_PROFILER_STATE_UNSPECIFIED;
        }

        /// @param status_line an HTTP status other than 200, or null.
        esp_err_t send_profiler_status(httpd_req_t *req, const char *status_line = nullptr)
        {
            // A build with no profiler answers the question rather than 404ing
            // it. "Nothing can run a pass here" is a fact a UI can render; a
            // missing route is one it has to guess about.
            const ProfilerStatus s = g_ctx.profiler ? g_ctx.profiler->profiler_status()
                                                    : ProfilerStatus{ProfilerState::UNAVAILABLE,
                                                                     0, 0, 0, 0, 0};

            pinled_v1_ProfilerStatus msg = pinled_v1_ProfilerStatus_init_zero;
            msg.state = wire_state(s.state);
            msg.window_ms = s.window_ms;
            msg.window_frames = s.window_frames;
            msg.frames_observed = s.frames_observed;
            msg.channels = s.channels;
            msg.passes = s.passes;

            uint8_t buf[pinled_v1_ProfilerStatus_size];
            pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
            if (!pb_encode(&os, pinled_v1_ProfilerStatus_fields, &msg))
                return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "encode failed");

            if (status_line)
                httpd_resp_set_status(req, status_line);
            return send_protobuf(req, buf, os.bytes_written);
        }

        esp_err_t get_profiler(httpd_req_t *req) { return send_profiler_status(req); }

        /// Re-arm (FR-PROF-2). Takes no body: there is nothing to say beyond
        /// "watch again", and the window is a device setting rather than a
        /// per-request one — a caller that could shorten it could ask for a
        /// classification that cannot be right.
        esp_err_t post_profiler(httpd_req_t *req)
        {
            // GET answers UNAVAILABLE with a 200 because a status endpoint
            // should always produce a status. POST fails instead: it is a
            // request to do something, nothing was done, and a 200 would say
            // otherwise.
            if (g_ctx.profiler == nullptr)
                return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "no scan task on this build");

            if (!g_ctx.profiler->rearm_profiler())
            {
                // 409 with the status as its body, rather than httpd's error
                // path: a second press while a pass is running has changed
                // nothing, and the useful reply is *which* pass is in the way
                // and how far through it is. httpd_resp_send_err would send a
                // plain-text sentence instead and lose that.
                ESP_LOGI(TAG, "profiler re-arm refused: a pass is already running");
                return send_profiler_status(req, "409 Conflict");
            }

            ESP_LOGI(TAG, "profiler re-armed by API request");
            // The status, not an ApplyResult: nothing was applied and nothing
            // restarted. The caller gets the window it is now waiting on.
            return send_profiler_status(req);
        }
        // --- OTA (FR-OTA-1/2/3) ----------------------------------------------

        /// Send an ApplyResult with @p status (null = 200). OTA's outcomes are
        /// all "accepted or not, and why" — exactly what the config apply path
        /// already answers with, so the SPA needs no new message type.
        esp_err_t send_ota_result(httpd_req_t *req, const char *status,
                                  bool accepted, const char *message)
        {
            pinled_v1_ApplyResult result = pinled_v1_ApplyResult_init_zero;
            result.accepted = accepted;
            result.restarted = false;
            std::snprintf(result.message, sizeof(result.message), "%s", message);

            uint8_t out[pinled_v1_ApplyResult_size];
            pb_ostream_t os = pb_ostream_from_buffer(out, sizeof(out));
            pb_encode(&os, pinled_v1_ApplyResult_fields, &result);

            if (status)
                httpd_resp_set_status(req, status);
            return send_protobuf(req, out, os.bytes_written);
        }

        /// "aB" -> 0xAB, or -1. No sscanf: a 64-character header is parsed in
        /// one pass with no locale and no surprises.
        int hex_byte(char hi, char lo)
        {
            auto nib = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int h = nib(hi), l = nib(lo);
            return (h < 0 || l < 0) ? -1 : (h << 4) | l;
        }

        /// Firmware upload (FR-OTA-1): raw image bytes streamed into the
        /// inactive slot. The optional X-Image-SHA256 header (64 hex chars)
        /// is the published hash the browser already verified (FR-OTA-3);
        /// when present, the device independently checks what actually
        /// arrived against it.
        ///
        /// Deliberately NOT the apply: success here means STAGED, and the
        /// response says so. Only the physical button makes it boot
        /// (FR-OTA-2), which is why open CORS on this endpoint is safe — the
        /// worst the network can do is fill a slot nobody boots.
        esp_err_t post_ota(httpd_req_t *req)
        {
            add_cors(req);
            if (g_ctx.ota == nullptr)
                return send_ota_result(req, "503 Service Unavailable", false,
                                       "no OTA support on this build");

            const size_t len = req->content_len;
            if (len == 0)
                return fail(req, HTTPD_400_BAD_REQUEST, "empty body");

            // The header is read before begin() so a malformed request is
            // refused before anything is erased.
            uint8_t sha[32];
            const uint8_t *expected = nullptr;
            {
                char hex[65];
                const esp_err_t got =
                    httpd_req_get_hdr_value_str(req, "X-Image-SHA256", hex, sizeof(hex));
                if (got == ESP_OK)
                {
                    for (int i = 0; i < 32; ++i)
                    {
                        const int b = hex_byte(hex[2 * i], hex[2 * i + 1]);
                        if (b < 0)
                            return fail(req, HTTPD_400_BAD_REQUEST,
                                        "X-Image-SHA256 is not 64 hex characters");
                        sha[i] = static_cast<uint8_t>(b);
                    }
                    expected = sha;
                }
                else if (got != ESP_ERR_NOT_FOUND)
                {
                    return fail(req, HTTPD_400_BAD_REQUEST,
                                "X-Image-SHA256 is not 64 hex characters");
                }
            }

            esp_err_t err = g_ctx.ota->begin(len);
            if (err == ESP_ERR_INVALID_STATE)
                return send_ota_result(req, "409 Conflict", false,
                                       "an upload or a staged image is already in progress");
            if (err == ESP_ERR_INVALID_SIZE)
                return send_ota_result(req, "413 Content Too Large", false,
                                       "image larger than the OTA slot");
            if (err != ESP_OK)
                return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not open the slot");

            // 4 KB: one flash write worth per recv, heap rather than the 6 KB
            // httpd stack. The same shape as put_config's body loop.
            constexpr size_t kChunk = 4096;
            std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[kChunk]);
            if (!buf)
            {
                g_ctx.ota->abort_upload();
                return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
            }

            size_t got_bytes = 0;
            int stalls = 0;
            while (got_bytes < len)
            {
                const size_t want = len - got_bytes < kChunk ? len - got_bytes : kChunk;
                const int n = httpd_req_recv(req, reinterpret_cast<char *>(buf.get()),
                                             want);
                if (n == HTTPD_SOCK_ERR_TIMEOUT)
                {
                    if (++stalls > 4)
                    {
                        g_ctx.ota->abort_upload();
                        return fail(req, HTTPD_408_REQ_TIMEOUT, "upload stalled");
                    }
                    continue;
                }
                if (n <= 0)
                {
                    g_ctx.ota->abort_upload();
                    return fail(req, HTTPD_400_BAD_REQUEST, "truncated upload");
                }
                stalls = 0;
                const esp_err_t werr = g_ctx.ota->write(buf.get(), static_cast<size_t>(n));
                if (werr != ESP_OK)
                {
                    g_ctx.ota->abort_upload();
                    // esp_ota_write checks the image magic on the first
                    // chunk, so "you sent me a PDF" is a caller error, not a
                    // flash failure, and it is caught kilobytes in rather
                    // than megabytes.
                    if (werr == ESP_ERR_OTA_VALIDATE_FAILED)
                        return send_ota_result(req, "400 Bad Request", false,
                                               "not a firmware image");
                    return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash write failed");
                }
                got_bytes += static_cast<size_t>(n);
            }

            err = g_ctx.ota->finish(expected);
            if (err == ESP_ERR_INVALID_CRC)
                return send_ota_result(req, "400 Bad Request", false,
                                       "SHA-256 mismatch: not the published image");
            if (err != ESP_OK)
                return send_ota_result(req, "400 Bad Request", false,
                                       "not a bootable image");

            char msg[64];
            std::snprintf(msg, sizeof(msg),
                          "staged; press the button within %u s to boot it",
                          (unsigned)((g_ctx.ota->remaining_ms() + 999) / 1000));
            return send_ota_result(req, nullptr, true, msg);
        }

        /// Discard a staged image — the safe direction, so the network may
        /// take it (FR-OTA-2). There is intentionally no API counterpart for
        /// confirm: that belongs to the button alone.
        esp_err_t delete_ota(httpd_req_t *req)
        {
            add_cors(req);
            if (g_ctx.ota == nullptr)
                return send_ota_result(req, "503 Service Unavailable", false,
                                       "no OTA support on this build");
            if (!g_ctx.ota->discard())
                return fail(req, HTTPD_404_NOT_FOUND, "nothing staged");
            return send_ota_result(req, nullptr, true, "staged image discarded");
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
                {
                    // Keep draining. The scan publishes whether or not anyone
                    // is watching, so skipping this lets the sticky bits
                    // accumulate for the whole gap between clients — and the
                    // first frame of a new session then reports activity that
                    // may be minutes old, on a channel that has been quiet
                    // throughout. A ghost lamp is a bad first impression and
                    // a worse thing to debug.
                    g_ctx.live->drain();
                    continue;
                }

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
                         LiveState &live,
                         ProfilerControl *profiler,
                         OtaManager *ota)
    {
        g_ctx.store = &store;
        g_ctx.running = &running;
        g_ctx.channels = channels;
        g_ctx.count = count;
        g_ctx.net = &net;
        // Scanning retunes the radio, so it cannot be const — but everything
        // else about Net is read-only from here, and the reference the caller
        // hands us says so.
        g_ctx.net_mutable = const_cast<Net *>(&net);
        g_ctx.live = &live;
        g_ctx.profiler = profiler;
        g_ctx.ota = ota;
        g_ctx.server = nullptr;

        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        // Well past the default cap of eight, which fails at registration time
        // with a log line nobody reads.
        cfg.max_uri_handlers = 32;
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
            // The classifier, not the document above. One letter apart, and
            // they share nothing.
            {"/api/v1/profiler", HTTP_GET, get_profiler},
            {"/api/v1/profiler", HTTP_POST, post_profiler},
            {"/api/v1/profiler", HTTP_OPTIONS, handle_options},
            {"/api/v1/ota", HTTP_POST, post_ota},
            {"/api/v1/ota", HTTP_DELETE, delete_ota},
            {"/api/v1/ota", HTTP_OPTIONS, handle_options},
            {"/api/v1/scan", HTTP_GET, get_scan},
            {"/api/v1/provision", HTTP_POST, post_provision},
            {"/api/v1/provision", HTTP_DELETE, delete_provision},
            {"/api/v1/provision", HTTP_OPTIONS, handle_options},
            // The portal itself, plus the URLs each OS probes to decide a
            // network is captive. Registering them explicitly rather than with
            // a wildcard keeps the API paths unambiguous.
            {"/", HTTP_GET, get_portal},
            {"/generate_204", HTTP_GET, redirect_to_portal},          // Android
            {"/gen_204", HTTP_GET, redirect_to_portal},               // Android, older
            {"/hotspot-detect.html", HTTP_GET, redirect_to_portal},   // iOS, macOS
            {"/library/test/success.html", HTTP_GET, redirect_to_portal},
            {"/connecttest.txt", HTTP_GET, redirect_to_portal},       // Windows
            {"/ncsi.txt", HTTP_GET, redirect_to_portal},              // Windows
            {"/canonical.html", HTTP_GET, redirect_to_portal},        // Firefox
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
