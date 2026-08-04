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
        // Production modules reset on the clock-idle timeout and have no /MR
        // conductor; a negative Kconfig value means "not fitted".
        c.mr_pin = CONFIG_PINLED_MR_GPIO >= 0
                       ? static_cast<gpio_num_t>(CONFIG_PINLED_MR_GPIO)
                       : GPIO_NUM_NC;
        c.data_pin = static_cast<gpio_num_t>(CONFIG_PINLED_DATA_GPIO);
        c.spi_hz = CONFIG_PINLED_SPI_HZ;
        c.spi_mode = CONFIG_PINLED_SPI_MODE;
#ifdef CONFIG_PINLED_MR_FROM_CS
        c.mr_from_cs = true;
#else
        c.mr_from_cs = false;
#endif
#ifdef CONFIG_PINLED_ARM_CLOCK
        c.arm_clock = true;
#else
        c.arm_clock = false;
#endif
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
        ESP_LOGI(TAG, "  chain: SPI %d Hz mode %u, /MR %s, arm clock %s",
                 out.spi_hz, (unsigned)out.spi_mode,
                 out.mr_pin == GPIO_NUM_NC ? "not fitted"
                                           : (out.mr_from_cs ? "via SPI CS" : "via GPIO"),
                 out.arm_clock ? "on" : "off");
        return ESP_OK;
    }

    esp_err_t MachineConfigStore::save(const MachineConfig &cfg)
    {
        (void)cfg;
        ESP_LOGW(TAG, "save() not yet implemented (v1)");
        return ESP_ERR_NOT_SUPPORTED;
    }
} // namespace ooe::pinled
