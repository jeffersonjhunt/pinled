#pragma once

/**
 * @file api.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief The device HTTP API (`/api/v1`).
 * @version 0.5.0
 * @date 2026-08-11
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * @par Endpoints
 * | Method | Path | Body |
 * |---|---|---|
 * | GET | `/api/v1/info` | `DeviceInfo` |
 * | GET | `/api/v1/config` | `InstallConfig` |
 * | PUT | `/api/v1/config` | `InstallConfig` → stored, then restart |
 * | DELETE | `/api/v1/config` | none → revert to build defaults |
 * | GET | `/api/v1/live` | WebSocket, `LiveFrame` at ~30 Hz |
 * | GET | `/api/v1/profile` | `MachineProfile` |
 * | PUT | `/api/v1/profile` | `MachineProfile` → stored, then restart |
 * | DELETE | `/api/v1/profile` | none → revert to unstyled |
 * | GET | `/api/v1/profiler` | `ProfilerStatus` |
 * | POST | `/api/v1/profiler` | none → re-arm, `ProfilerStatus` |
 * | POST | `/api/v1/ota` | raw image bytes → staged, `ApplyResult` |
 * | DELETE | `/api/v1/ota` | none → discard staged, `ApplyResult` |
 *
 * @par `profile` and `profiler` are different things
 * `/profile` is the stored `MachineProfile` document — which lamps are named
 * and styled. `/profiler` is the drive-scheme classifier, which touches no
 * stored bytes at all. They are one letter apart and share nothing, which is
 * worth knowing before reading either handler.
 *
 * Bodies are protobuf (`application/x-protobuf`); the firmware contains no
 * JSON parser (FR-CFG-13). `tools/pbtool.py` is the `curl` equivalent.
 *
 * @par GET returns the stored bytes, unmodified
 * When a document exists it is streamed back **verbatim** — the CRC is
 * verified and the payload is copied out without ever being decoded. That is
 * what makes the FR-UI-4 version window real rather than aspirational: a
 * document written by a newer SPA carries fields this firmware has no struct
 * for, and re-encoding would drop every one of them. It also means
 * `GET /profile` never materialises lamp names on the device, which is what
 * the two-model split in step 2 was for.
 *
 * Only when nothing is stored does `GET /config` synthesise a document from
 * the running configuration, so a fresh device can still say what it is doing.
 * `GET /profile` on an unconfigured device returns an empty profile, because
 * "no lamps are styled" is the truth and inventing entries would not be.
 *
 * @par Applying is write-then-restart
 * FR-CFG-16. The response is flushed first and the restart follows on a timer,
 * so the client sees `200` rather than a dropped connection. Re-initialising
 * the scan device, integrators and renderer in place is not worth the failure
 * modes for an operation performed at commissioning time.
 *
 * @par Unauthenticated, and CORS wide open
 * FR-UI-3, deliberately. The SPA may be a standalone bundle opened from
 * `file://`, whose origin is `null`, so nothing narrower would work. There is
 * no secret on the device to protect (FR-REG-1) and no cloud account behind
 * it. The one endpoint where that would be dangerous is OTA, and FR-OTA-2
 * gates it behind a physical button press instead: the API can stage an image
 * and discard one — the harmless directions — but only the button can make
 * the device boot it. Open CORS therefore costs a slot nobody boots, at worst.
 *
 * @par The live socket
 * `/api/v1/live` upgrades to a WebSocket carrying a `LiveFrame` about thirty
 * times a second (FR-UI-5). The push task runs at low priority off the scan
 * core and **drops frames rather than waiting** (FR-UI-9) — the sticky
 * activity bit in `pinled_live.h` is what makes that free, because a dropped
 * push is a longer window rather than a lost sample.
 *
 * Note that `application/x-protobuf` is not a CORS-safelisted request content
 * type, so every browser `PUT` is preceded by an `OPTIONS` preflight. Handling
 * it is not optional: without it writes fail in a browser while working
 * perfectly from `pbtool`, which is a miserable thing to debug.
 */

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

#include "machine_config.h"
#include "net.h"
#include "pinled_live.h"
#include "pinled_profiling.h"

namespace ooe::pinled
{
    /// Staging and discard, from components/pinled_ota. Forward-declared so
    /// this header does not pull the OTA stack into every includer; only the
    /// pointer crosses here.
    class OtaManager;

    /**
     * @brief What the API is allowed to ask of the auto-profiler.
     *
     * An interface rather than a pointer to `Main` because the dependency
     * would otherwise run the wrong way: the application knows about the API,
     * and the API knowing about the application in return makes both
     * untestable and neither replaceable. Two methods is the whole surface.
     */
    class ProfilerControl
    {
    public:
        virtual ~ProfilerControl() = default;

        /**
         * @brief Request a fresh classification pass.
         * @return false if one is already running, or if nothing can run one.
         *
         * Returns as soon as the request is posted — the pass itself takes the
         * observation window, and the caller watches `status()` or the live
         * socket to see it land.
         */
        virtual bool rearm_profiler() = 0;

        /// Sampled without a lock; see the note on ProfilerStatus.
        virtual ProfilerStatus profiler_status() const = 0;
    };

    /**
     * @brief API version, distinct from the firmware version (FR-UI-4).
     *
     * A cloud-hosted SPA branches on this, so it changes when the *contract*
     * changes — a new endpoint, a changed status code, a field that stops
     * meaning what it did — and not when the firmware is rebuilt. Adding a
     * field to the schema is not a change here: protobuf already handles that.
     */
    inline constexpr uint32_t kApiVersion = 1;

    class Api
    {
    public:
        /**
         * @brief Start the HTTP server.
         *
         * @param store   the configuration store, for reads and writes
         * @param running the configuration currently in force, for `GET`
         *                on an unconfigured device and for `DeviceInfo`
         * @param channels the running per-channel records; may be null
         * @param count   entries in @p channels
         * @param net     for reporting how the device was reached
         * @param live    the scan → push hand-off the monitor reads
         * @param profiler re-arm and status; may be null, in which case
         *                 `/api/v1/profiler` reports UNAVAILABLE rather than
         *                 404 — the endpoint existing and saying "not on this
         *                 build" is a better answer to a UI than absence
         * @param ota     staging and discard; may be null, in which case
         *                 `/api/v1/ota` answers 503 for the same reason
         */
        esp_err_t start(MachineConfigStore &store,
                        const MachineConfig &running,
                        const ChannelConfig *channels, size_t count,
                        const Net &net,
                        LiveState &live,
                        ProfilerControl *profiler,
                        OtaManager *ota);

        void stop();

    private:
        void *server_{nullptr}; ///< httpd_handle_t
    };
} // namespace ooe::pinled
