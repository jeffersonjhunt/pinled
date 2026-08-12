/**
 * @file machine_config.cpp
 * @brief The configuration store: LittleFS documents, Kconfig defaults.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "machine_config.h"

#include <cstdio>
#include <cstring>
#include <memory>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_system.h" // esp_get_free_heap_size
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "pb_decode.h"
#include "pinled.pb.h"
#include "pinled_apply.h" // default_channels
#include "pinled_doc_file.h"
#include "pinled_resolve.h"

namespace ooe::pinled
{
    static const char *TAG = "machine_config";

    namespace
    {
        const char *path_for(DocKind kind)
        {
            switch (kind)
            {
            case DocKind::InstallConfig:  return kInstallPath;
            case DocKind::MachineProfile: return kProfilePath;
            default:                      return nullptr;
            }
        }

        /// The largest framed document this schema can produce. nanopb computes
        /// the message size from `pinled.options`, so tightening a bound there
        /// shrinks this buffer automatically rather than leaving a stale
        /// constant behind.
        constexpr size_t kReadBufBytes = doc_frame_size(pinled_v1_MachineProfile_size);

        bool decode_into(const uint8_t *payload, size_t len,
                         const pb_msgdesc_t *fields, void *dest)
        {
            pb_istream_t s = pb_istream_from_buffer(payload, len);
            return pb_decode(&s, fields, dest);
        }

        // Undo a failed decode. The generated init_zero macros are typed, so
        // the table below carries a function per document rather than a memset
        // — which would be right today and wrong the moment a field needs
        // anything other than zero to mean "unset".
        void clear_install(void *p)
        {
            *static_cast<pinled_v1_InstallConfig *>(p) = pinled_v1_InstallConfig_init_zero;
        }
        void clear_profile(void *p)
        {
            *static_cast<pinled_v1_MachineProfile *>(p) = pinled_v1_MachineProfile_init_zero;
        }
    } // namespace

    MachineConfig MachineConfigStore::defaults()
    {
        MachineConfig c{};
        c.clk_pin = CONFIG_PINLED_CLK_GPIO;
        // /PL is bussed to every '165 and is not optional on rev D hardware:
        // without it the registers never freeze and never shift. A negative
        // Kconfig value means "not fitted", which is a bench-rig-only state.
        c.pl_pin = CONFIG_PINLED_PL_GPIO >= 0 ? CONFIG_PINLED_PL_GPIO : kPinNotFitted;
        c.data_pin = CONFIG_PINLED_DATA_GPIO;
        c.spi_hz = CONFIG_PINLED_SPI_HZ;
        c.spi_mode = CONFIG_PINLED_SPI_MODE;
#ifdef CONFIG_PINLED_PL_FROM_CS
        c.pl_from_cs = true;
#else
        c.pl_from_cs = false;
#endif
        c.led_pin = CONFIG_PINLED_LED_GPIO;
        c.num_modules = CONFIG_PINLED_NUM_MODULES;
        c.channels_per_module = CONFIG_PINLED_CHANNELS_PER_MODULE;
#ifdef CONFIG_PINLED_ACTIVE_LOW
        c.active_low = true;
#else
        c.active_low = false;
#endif
        c.sample_rate_hz = static_cast<float>(CONFIG_PINLED_SAMPLE_RATE_HZ);
        c.refresh_hz = CONFIG_PINLED_REFRESH_HZ;
        c.attack_ms = static_cast<float>(CONFIG_PINLED_ATTACK_MS);
        c.decay_ms = static_cast<float>(CONFIG_PINLED_DECAY_MS);
        c.led_count = CONFIG_PINLED_LED_COUNT;
        return c;
    }

    esp_err_t MachineConfigStore::mount()
    {
        if (mounted_)
            return ESP_OK;

        esp_vfs_littlefs_conf_t conf{};
        conf.base_path = kCfgMount;
        conf.partition_label = kCfgPartition;
        conf.dont_mount = false;

        // Two attempts, and the split matters. A partition that has never held
        // a filesystem is the state every board leaves the factory in, so
        // formatting it is initialisation. A partition that holds a filesystem
        // too damaged to mount is a different event entirely, and formatting
        // THAT discards someone's configuration.
        //
        // Asking for the mount first tells the two apart, so the destructive
        // case is at least announced. It is still done — an unmountable config
        // partition must not leave the device unable to boot the UI that could
        // replace it (FR-CFG-4) — but "your configuration was discarded" is a
        // sentence the log should contain, and a single call with
        // format_if_mount_failed would never print it.
        conf.format_if_mount_failed = false;
        esp_err_t err = esp_vfs_littlefs_register(&conf);

        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "storage did not mount (%s); formatting", esp_err_to_name(err));
            if (esp_littlefs_format(kCfgPartition) == ESP_OK)
                ESP_LOGW(TAG, "storage formatted — any stored configuration is gone");
            err = esp_vfs_littlefs_register(&conf);
        }
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
            return err;
        }

        size_t total = 0, used = 0;
        if (esp_littlefs_info(kCfgPartition, &total, &used) == ESP_OK)
            ESP_LOGI(TAG, "storage mounted at %s: %u of %u bytes used",
                     kCfgMount, (unsigned)used, (unsigned)total);

        mounted_ = true;
        return ESP_OK;
    }

    esp_err_t MachineConfigStore::load(MachineConfig &out,
                                       ChannelConfig *channels, size_t cap, size_t *count)
    {
        if (count != nullptr)
            *count = 0;

        state_ = StoreState{};

        // NVS is still initialised here, and still holds nothing. It is the
        // right home for the small key-value settings that come with the UI —
        // Wi-Fi credentials and the author handle (FR-CFG-7/12) — and not for
        // the documents, which is what the filesystem below is for.
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
        {
            ESP_LOGW(TAG, "erasing NVS (%s)", esp_err_to_name(err));
            ESP_ERROR_CHECK(nvs_flash_erase());
            err = nvs_flash_init();
        }
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
            return err;
        }

        const MachineConfig kconfig = defaults();
        out = kconfig;

        ResolveDefaults rd{};
        rd.attack_ms = static_cast<uint16_t>(kconfig.attack_ms);
        rd.decay_ms = static_cast<uint16_t>(kconfig.decay_ms);

        state_.mounted = mount() == ESP_OK;

        // Both decoded documents and the read buffer are alive together only
        // inside this scope, and every one of them is large. Holding them as
        // members would cost that memory for the life of the process, for a
        // few milliseconds of use at boot.
        std::unique_ptr<pinled_v1_InstallConfig> install;
        std::unique_ptr<pinled_v1_MachineProfile> profile;

        // Measured, not estimated. The two documents and the read buffer are
        // the largest thing this firmware allocates, and M4 puts Wi-Fi, lwIP
        // and httpd on top of what survives — so the number is reported rather
        // than reasoned about. (The first version of this comment guessed
        // 45 KB; the arithmetic said 17 KB and the board settles it.)
        const uint32_t heap_before = esp_get_free_heap_size();

        if (state_.mounted)
        {
            std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[kReadBufBytes]);
            install.reset(new (std::nothrow) pinled_v1_InstallConfig());
            profile.reset(new (std::nothrow) pinled_v1_MachineProfile());

            if (!buf || !install || !profile)
            {
                ESP_LOGE(TAG, "out of memory reading configuration; using defaults");
                install.reset();
                profile.reset();
            }
            else
            {
                // Everything large is now allocated, so this is the peak —
                // measured directly rather than inferred from a since-boot
                // low-water mark, which would only be the same number if
                // nothing before this point ever went deeper.
                ESP_LOGI(TAG, "decode buffers: %u bytes transient (freed before boot completes)",
                         (unsigned)(heap_before - esp_get_free_heap_size()));

                *install = pinled_v1_InstallConfig_init_zero;
                *profile = pinled_v1_MachineProfile_init_zero;

                struct
                {
                    DocKind kind;
                    const char *path;
                    const pb_msgdesc_t *fields;
                    void *dest;
                    void (*clear)(void *);
                    bool *present;
                    bool *rejected;
                } docs[] = {
                    {DocKind::InstallConfig, kInstallPath, pinled_v1_InstallConfig_fields,
                     install.get(), clear_install,
                     &state_.install_present, &state_.install_rejected},
                    {DocKind::MachineProfile, kProfilePath, pinled_v1_MachineProfile_fields,
                     profile.get(), clear_profile,
                     &state_.profile_present, &state_.profile_rejected},
                };

                for (auto &d : docs)
                {
                    DocKind kind = DocKind::Unknown;
                    const uint8_t *payload = nullptr;
                    size_t payload_len = 0;
                    DocStatus ds = DocStatus::Ok;

                    const FileStatus fs = doc_file_read(d.path, buf.get(), kReadBufBytes,
                                                        &kind, &payload, &payload_len, &ds);
                    if (fs == FileStatus::NotFound)
                    {
                        ESP_LOGI(TAG, "%s absent; using defaults", d.path);
                        continue;
                    }
                    if (fs != FileStatus::Ok)
                    {
                        // Loud, and kept. See the class note: the file stays on
                        // flash so there is something to look at afterwards.
                        ESP_LOGE(TAG, "%s rejected: %s (%s) — KEPT, using defaults",
                                 d.path, file_status_str(fs), doc_status_str(ds));
                        *d.rejected = true;
                        continue;
                    }
                    if (kind != d.kind)
                    {
                        ESP_LOGE(TAG, "%s holds document kind %u, expected %u — using defaults",
                                 d.path, (unsigned)kind, (unsigned)d.kind);
                        *d.rejected = true;
                        continue;
                    }
                    if (!decode_into(payload, payload_len, d.fields, d.dest))
                    {
                        // nanopb leaves whatever it managed to decode behind,
                        // and a rejected document must be indistinguishable
                        // from an absent one downstream. The reachable case is
                        // the one FR-CFG-5 is about: a shared profile from a
                        // bigger machine, where nanopb fills 128 lamps, hits
                        // max_count and fails — leaving 128 real entries that
                        // would otherwise go on to style this playfield.
                        d.clear(d.dest);
                        ESP_LOGE(TAG, "%s failed to decode — using defaults", d.path);
                        *d.rejected = true;
                        continue;
                    }
                    *d.present = true;
                    ESP_LOGI(TAG, "%s loaded (%u byte payload)", d.path, (unsigned)payload_len);
                }
            }
        }

        if (state_.install_present)
        {
            const InstallStatus is = install_to_machine(*install, out, kconfig);
            if (is != InstallStatus::Ok)
            {
                ESP_LOGE(TAG, "install config unusable: %s — using defaults",
                         install_status_str(is));
                state_.install_present = false;
                state_.install_rejected = true;
                out = kconfig;
            }
        }

        if (state_.profile_present && !state_.install_present)
        {
            // The profile is keyed by lamp number and the install is the only
            // thing that says which channel a lamp is on. Without it there is
            // nothing to bind the styling to, and pretending otherwise would
            // apply someone's colours to arbitrary channels.
            ESP_LOGW(TAG, "profile present but no usable install config; "
                          "nothing to bind lamps to");
        }

        // Refused, not truncated. Clamping here defeated main's own check —
        // it can only ever see a count that already fits — while `out` went on
        // carrying the oversized geometry into scan_.init() and filament_.init().
        // A geometry this large can only come from Kconfig, since
        // install_to_machine() rejects it, so it is a build error and should
        // say so rather than quietly running half a machine.
        if (out.total_channels() > cap)
        {
            ESP_LOGE(TAG, "geometry needs %u channels, only %u available",
                     (unsigned)out.total_channels(), (unsigned)cap);
            return ESP_ERR_INVALID_SIZE;
        }

        if (channels != nullptr)
        {
            size_t n = 0;
            bool resolved = false;

            if (state_.install_present)
            {
                const ResolveStatus rs = resolve(*profile, *install, channels, cap, &n, rd);
                if (rs == ResolveStatus::Ok)
                    resolved = true;
                else
                    ESP_LOGE(TAG, "resolve failed: %s — using defaults",
                             resolve_status_str(rs));
            }

            if (!resolved)
            {
                n = out.total_channels();
                default_channels(channels, n, out.led_count, rd);
            }
            if (count != nullptr)
                *count = n;
        }
        else if (count != nullptr)
        {
            *count = out.total_channels();
        }

        // Keyed on the install alone, not on state_.defaulted(). This line
        // describes the numbers printed beside it, and every one of them comes
        // from the install document — a valid profile alongside a rejected
        // install would otherwise have this say "stored" about a configuration
        // that is entirely defaults. Found on hardware by corrupting a
        // document on purpose, which is the only reason it was ever seen.
        ESP_LOGI(TAG, "config: %u module(s) x %u ch, %.0f Hz sample, %u Hz refresh (%s)",
                 (unsigned)out.num_modules, (unsigned)out.channels_per_module,
                 out.sample_rate_hz, (unsigned)out.refresh_hz,
                 state_.install_present ? "stored" : "build defaults");
        ESP_LOGI(TAG, "  chain: SPI %d Hz mode %u, /PL %s",
                 (int)out.spi_hz, (unsigned)out.spi_mode,
                 out.pl_pin == kPinNotFitted ? "not fitted"
                                             : (out.pl_from_cs ? "via SPI CS" : "via GPIO"));
        return ESP_OK;
    }

    esp_err_t MachineConfigStore::save_document(DocKind kind,
                                                const uint8_t *payload, size_t len)
    {
        const char *path = path_for(kind);
        if (path == nullptr)
            return ESP_ERR_INVALID_ARG;
        if (payload == nullptr && len != 0)
            return ESP_ERR_INVALID_ARG;

        const esp_err_t merr = mount();
        if (merr != ESP_OK)
            return merr;

        const FileStatus fs = doc_file_write(path, kind, payload, len);
        if (fs != FileStatus::Ok)
        {
            ESP_LOGE(TAG, "writing %s failed: %s", path, file_status_str(fs));
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "wrote %s (%u byte payload)", path, (unsigned)len);
        return ESP_OK;
    }

    esp_err_t MachineConfigStore::erase_document(DocKind kind)
    {
        const char *path = path_for(kind);
        if (path == nullptr)
            return ESP_ERR_INVALID_ARG;

        const esp_err_t merr = mount();
        if (merr != ESP_OK)
            return merr;

        if (std::remove(path) != 0 && doc_file_exists(path))
        {
            ESP_LOGE(TAG, "removing %s failed", path);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "removed %s", path);
        return ESP_OK;
    }

#ifdef CONFIG_PINLED_STORE_SELFTEST
    esp_err_t MachineConfigStore::selftest()
    {
        const esp_err_t merr = mount();
        if (merr != ESP_OK)
            return merr;

        static constexpr char kPath[] = "/cfg/selftest.pb";
        // Not protobuf, on purpose: this exercises the frame and the medium,
        // and a payload the decoder would accept would only invite someone to
        // think it tests the decoder too.
        const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03};

        FileStatus fs = doc_file_write(kPath, DocKind::Unknown, payload, sizeof(payload));
        if (fs != FileStatus::Ok)
        {
            ESP_LOGE(TAG, "selftest: write failed: %s", file_status_str(fs));
            return ESP_FAIL;
        }

        uint8_t buf[64];
        DocKind kind = DocKind::InstallConfig; // deliberately not the value expected
        const uint8_t *got = nullptr;
        size_t got_len = 0;
        DocStatus ds = DocStatus::Ok;

        fs = doc_file_read(kPath, buf, sizeof(buf), &kind, &got, &got_len, &ds);
        std::remove(kPath);

        if (fs != FileStatus::Ok)
        {
            ESP_LOGE(TAG, "selftest: read back failed: %s (%s)",
                     file_status_str(fs), doc_status_str(ds));
            return ESP_FAIL;
        }
        if (kind != DocKind::Unknown || got_len != sizeof(payload) ||
            std::memcmp(got, payload, sizeof(payload)) != 0)
        {
            ESP_LOGE(TAG, "selftest: read back %u bytes, kind %u — MISMATCH",
                     (unsigned)got_len, (unsigned)kind);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "selftest: write, sync, rename and read back agree (%u bytes)",
                 (unsigned)got_len);
        return ESP_OK;
    }
#endif
} // namespace ooe::pinled
