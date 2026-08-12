#pragma once

/**
 * @file machine_config.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief The configuration store: LittleFS documents, Kconfig defaults.
 * @version 0.4.0
 * @date 2026-08-10
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * Owns the two stored documents (FR-CFG-7) and turns them into the two runtime
 * records the rest of the firmware reads: `MachineConfig` for the device and
 * `ChannelConfig[]` for the channels.
 *
 * @par What is here and what is not
 * Almost none of the work is: framing and integrity are `pinled_doc_frame`,
 * file I/O is `pinled_doc_file`, and the projections are `pinled_resolve`.
 * All of those are IDF-free and tested on the host. What is left here is the
 * part that genuinely needs the target — mounting LittleFS, reading Kconfig,
 * and logging — which is why this file is thin and deliberately dull.
 *
 * @par Boot never fails because of storage
 * A missing document is the normal state of a device that has never been
 * configured, and a corrupt one is a fault the user needs to hear about but
 * not be stranded by. Either way boot continues on Kconfig defaults
 * (FR-CFG-4). There is no state in which a bad file on flash prevents the
 * firmware from coming up and serving the UI that could replace it.
 *
 * @par Corrupt documents are kept
 * A rejected file is logged and left exactly where it is. Deleting it would
 * destroy the only evidence of what went wrong, and it costs nothing: it is
 * never read again, and the next successful write replaces it atomically.
 */

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

#include "pinled_channel_config.h"
#include "pinled_doc_frame.h"
#include "pinled_machine_config.h"

namespace ooe::pinled
{
    /// Where the configuration filesystem is mounted, and what lives in it.
    /// Paths are short on purpose: LittleFS stores the full name in metadata.
    inline constexpr char kCfgMount[] = "/cfg";
    inline constexpr char kInstallPath[] = "/cfg/install.pb";
    inline constexpr char kProfilePath[] = "/cfg/profile.pb";

    /// Label of the data partition in partitions.csv.
    inline constexpr char kCfgPartition[] = "storage";

    /// What `load()` actually found, for logging and for the API to report.
    /// Distinct from `esp_err_t` because none of these are failures: they are
    /// descriptions of a device's state.
    struct StoreState
    {
        bool mounted{false};
        bool install_present{false}; ///< a valid install document was applied
        bool profile_present{false}; ///< a valid profile document was applied
        bool install_rejected{false};
        bool profile_rejected{false};

        /// True when the running configuration is entirely Kconfig defaults.
        bool defaulted() const { return !install_present && !profile_present; }
    };

    class MachineConfigStore
    {
    public:
        /**
         * @brief Bring up storage and load the active configuration.
         *
         * Mounts LittleFS, reads both documents, and projects them onto the
         * two runtime records. Falls back to Kconfig defaults for whatever is
         * absent or rejected, so this returns `ESP_OK` on a blank device.
         *
         * @param out       receives the device-wide record
         * @param channels  receives the per-channel records; may be null
         * @param cap       capacity of @p channels in elements
         * @param count     receives the channel count actually written
         *
         * @return ESP_OK unless something is wrong with the caller's arguments
         *         or NVS itself. Storage faults are reported in `state()`, not
         *         as an error — see the class note.
         *
         * @note Both documents and the read buffer are heap and not members:
         *       they are the largest thing this firmware allocates and they
         *       are needed for a few milliseconds at boot. The peak is
         *       measured and logged at boot (the "decode buffers" line) rather
         *       than claimed here — M4 puts Wi-Fi, lwIP and httpd on top of
         *       what survives, and the shipping board has no PSRAM (NFR-8).
         */
        esp_err_t load(MachineConfig &out,
                       ChannelConfig *channels, size_t cap, size_t *count);

        /**
         * @brief Store a document exactly as received.
         *
         * Takes encoded bytes rather than a `MachineConfig`, because the
         * device is not the author of its own configuration: the UI composes a
         * document and the device keeps it (FR-CFG-16). Re-encoding from the
         * runtime record would silently drop every field this firmware version
         * does not understand, which is precisely what the schema's
         * unknown-field preservation exists to avoid (FR-UI-4).
         *
         * The write is atomic; see `pinled_doc_file.h`.
         *
         * @param kind     which document
         * @param payload  encoded protobuf
         * @param len      payload length
         */
        esp_err_t save_document(DocKind kind, const uint8_t *payload, size_t len);

        /// Delete a stored document, reverting that half to defaults on the
        /// next boot. Absent is success — the caller asked for it to be gone.
        esp_err_t erase_document(DocKind kind);

        /// What the last `load()` found.
        const StoreState &state() const { return state_; }

        /// Fill a config from Kconfig (CONFIG_PINLED_*) build defaults.
        static MachineConfig defaults();

        /// Mount the configuration filesystem, formatting it if it has never
        /// been used. Called by `load()`; separate so a diagnostic can run
        /// before any configuration is read.
        esp_err_t mount();

#ifdef CONFIG_PINLED_STORE_SELFTEST
        /// Write, read back, compare and remove a scratch document on the real
        /// medium. Reports through the log; returns ESP_OK only if every step
        /// agreed. See the Kconfig help for why this is not host-testable.
        esp_err_t selftest();
#endif

    private:
        StoreState state_{};
        bool mounted_{false};
    };
} // namespace ooe::pinled
