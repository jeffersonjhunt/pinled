#include "main.h"

namespace ooe::pinled
{
    static const char *TAG = "pinled-main"; // tag for logging

    Main *pinled_main;

    /**
     * @brief bootstraps the application
     */
    extern "C" void app_main()
    {
        pinled_main = new Main();
        ESP_ERROR_CHECK(pinled_main->init());
        while (true)
        {
            pinled_main->run();
        }
        delete pinled_main;
    }

    /**
     * @brief initializes the application
     *
     * @return esp_err_t
     */
    esp_err_t Main::init()
    {
        esp_log_level_set("*", ESP_LOG_DEBUG);
        pinled_main->version();

        esp_err_t err{ESP_OK};
        ESP_LOGI(TAG, "Initializing...");

        ESP_LOGI(TAG, "Initializing complete.");
        return err;
    }

    /**
     * @brief runs the application
     */
    void Main::run()
    {
        ESP_LOGI(TAG, "tick");
        vTaskDelay(1000 * 15 / portTICK_PERIOD_MS);
    }

    /**
     * @brief logs the version of the application
     */
    void Main::version()
    {
        ESP_LOGI(TAG, "%s version: %i.%i.%i", PROJECT_NAME, PROJECT_VERSION_MAJOR, PROJECT_VERSION_MINOR, PROJECT_VERSION_PATCH);
    }
} // namespace pinled
