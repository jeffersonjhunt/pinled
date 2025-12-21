#include "main.h"

#define PIXEL_COUNT  5
#define NEOPIXEL_PIN GPIO_NUM_15

#if !defined(MAX)
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#endif
#define ARRAY_SIZE(x) (sizeof(x)/sizeof(x[0]))

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
        vTaskDelay(1000 * 1 / portTICK_PERIOD_MS);
        test1(1);
        test2(1);
    }

    /**
     * @brief logs the version of the application
     */
    void Main::version()
    {
        ESP_LOGI(TAG, "%s version: %i.%i.%i", PROJECT_NAME, PROJECT_VERSION_MAJOR, PROJECT_VERSION_MINOR, PROJECT_VERSION_PATCH);
    }


bool Main::test1(uint32_t iterations)
{
   tNeopixelContext neopixel = neopixel_Init(PIXEL_COUNT, NEOPIXEL_PIN);
   tNeopixel pixel[] =
   {
       { 0, NP_RGB(50, 0,  0) }, /* red */
       { 0, NP_RGB(0,  50, 0) }, /* green */
       { 0, NP_RGB(0,  0, 50) }, /* blue */
       { 0, NP_RGB(0,  0,  0) }, /* off */
   };

   if(NULL == neopixel)
   {
      ESP_LOGE(TAG, "[%s] Initialization failed\n", __func__);
      return false;
   }

   ESP_LOGI(TAG, "[%s] Starting", __func__);
   for(int iter = 0; iter < iterations; ++iter)
   {
      for(int i = 0; i < ARRAY_SIZE(pixel); ++i)
      {
         neopixel_SetPixel(neopixel, &pixel[i], 1);
         vTaskDelay(pdMS_TO_TICKS(200));
      }
   }
   ESP_LOGI(TAG, "[%s] Finished", __func__);

   neopixel_Deinit(neopixel);
   return true;
}

bool Main::test2(uint32_t iterations)
{
   tNeopixelContext neopixel = neopixel_Init(PIXEL_COUNT, NEOPIXEL_PIN);
   uint32_t refreshRate, taskDelay;

   if(NULL == neopixel)
   {
      ESP_LOGE(TAG, "[%s] Initialization failed\n", __func__);
      return false;
   }

   refreshRate = neopixel_GetRefreshRate(neopixel);
   taskDelay = MAX(1, pdMS_TO_TICKS(1000UL / refreshRate));
   ESP_LOGI(TAG, "[%s] Starting", __func__);
   for(uint32_t i = 0; i < iterations * PIXEL_COUNT; ++i)
   {
      tNeopixel pixel[] =
      {
          { (i)   % PIXEL_COUNT, NP_RGB(0, 0,  0) },
          { (i+5) % PIXEL_COUNT, NP_RGB(0, 50, 0) }, /* green */
      };
      neopixel_SetPixel(neopixel, pixel, ARRAY_SIZE(pixel));
      vTaskDelay(taskDelay);
   }
   ESP_LOGI(TAG, "[%s] Finished", __func__);
   neopixel_Deinit(neopixel);
   return true;
}



} // namespace pinled
