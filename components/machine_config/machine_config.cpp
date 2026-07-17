/**
 * @file machine_config.cpp
 * @brief Machine profile store (first-cut: Kconfig defaults + NVS init).
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * TODO(v1): serialize MachineConfig + per-channel color/lock tables to an NVS
 * blob; multiple named profiles; JSON import/export over console/USB.
 */

#include "machine_config.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace ooe::pinled
{
    static const char *TAG = "machine_config";

    MachineConfig MachineConfigStore::defaults()
    {
        MachineConfig c{};
        c.clk_pin = static_cast<gpio_num_t>(CONFIG_PINLED_CLK_GPIO);
        c.mr_pin = static_cast<gpio_num_t>(CONFIG_PINLED_MR_GPIO);
        c.data_pins[0] = static_cast<gpio_num_t>(CONFIG_PINLED_DATA0_GPIO);
        c.led_pin = static_cast<gpio_num_t>(CONFIG_PINLED_LED_GPIO);
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

    esp_err_t MachineConfigStore::load(MachineConfig &out)
    {
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

        // TODO(v1): read stored profile blob here. For now, defaults.
        out = defaults();
        ESP_LOGI(TAG, "loaded defaults: %u module(s) x %u ch, %.0f Hz sample, %u Hz refresh",
                 (unsigned)out.num_modules, (unsigned)out.channels_per_module,
                 out.sample_rate_hz, (unsigned)out.refresh_hz);
        return ESP_OK;
    }

    esp_err_t MachineConfigStore::save(const MachineConfig &cfg)
    {
        (void)cfg;
        ESP_LOGW(TAG, "save() not yet implemented (v1)");
        return ESP_ERR_NOT_SUPPORTED;
    }
} // namespace ooe::pinled
