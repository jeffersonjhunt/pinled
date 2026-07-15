#include "main.h"

/*
 * https://learn.adafruit.com/adafruit-qt-py-esp32-pico/pinouts
*/

#define PIXEL_COUNT  10 // Should come from config

#define NEOPIXEL_CTRL_PIN GPIO_NUM_15     // A3

#define COUNTER_CLK_PIN GPIO_NUM_25       // A1
#define COUNTER_RESET_PIN GPIO_NUM_27     // A2

#define MUX_DATA_IN_PIN GPIO_NUM_26       // A0

#define UNUSED_PIN_3 GPIO_NUM_4  // SDA
#define UNUSED_PIN_4 GPIO_NUM_33 // SCL
#define UNUSED_PIN_5 GPIO_NUM_32 // TX

#define UNUSED_PIN_6 GPIO_NUM_13   // MOSI
#define UNUSED_PIN_7 GPIO_NUM_12   // MISO
#define UNUSED_PIN_8 GPIO_NUM_14   // SCK
#define UNUSED_PIN_9 GPIO_NUM_7    // RX


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

        pinId = 0;

        gpio_reset_pin(COUNTER_CLK_PIN);
        gpio_set_direction(COUNTER_CLK_PIN, GPIO_MODE_OUTPUT);
        
        gpio_reset_pin(MUX_DATA_IN_PIN);
        gpio_set_direction(MUX_DATA_IN_PIN, GPIO_MODE_INPUT);

        gpio_reset_pin(COUNTER_RESET_PIN);
        gpio_set_direction(COUNTER_RESET_PIN, GPIO_MODE_OUTPUT);
        counter_reset();

        ESP_LOGI(TAG, "Initializing complete.");
        return err;
    }

    /**
     * @brief runs the application
     */
    void Main::run()
    {
        ESP_LOGI(TAG, "tick");

        // test1(5);
        // test2(50);
        // breath();
        // chase();
        count();
        check_state();

        vTaskDelay(100 * 1 / portTICK_PERIOD_MS);
      }

    /**
     * @brief logs the version of the application
     */
    void Main::version()
    {
        ESP_LOGI(TAG, "%s version: %i.%i.%i", PROJECT_NAME, PROJECT_VERSION_MAJOR, PROJECT_VERSION_MINOR, PROJECT_VERSION_PATCH);
    }

    bool Main::count()
    {
      gpio_set_level(COUNTER_CLK_PIN, 1);   // high
      gpio_set_level(COUNTER_CLK_PIN, 0);   // low

      return true;
    }

    bool Main::check_state()
    {
      bool isHigh = false;
      
      int state = gpio_get_level(MUX_DATA_IN_PIN);

      if(state == 1){
         isHigh = true;
         ESP_LOGI(TAG, "pin %i is logic high", pinId);
      }
      else
      {
         ESP_LOGI(TAG, "pin %i is logic low", pinId);
      }

      pinId++;
      if(pinId > 7){
         pinId = 0;
         counter_reset();
      } 

      return isHigh;
    }

    bool Main::counter_reset()
    {
      gpio_set_level(COUNTER_RESET_PIN, 0);   // low clear counter

      gpio_set_level(COUNTER_RESET_PIN, 1);   // high enable counter

      return true;
    }

    bool Main::breath()
    {
       tNeopixelContext neopixel = neopixel_Init(PIXEL_COUNT, NEOPIXEL_CTRL_PIN);
       uint32_t refreshRate, taskDelay;

       if(NULL == neopixel)
       {
          ESP_LOGE(TAG, "[%s] Initialization failed\n", __func__);
          return false;
       }

       refreshRate = neopixel_GetRefreshRate(neopixel);
       taskDelay = MAX(1, pdMS_TO_TICKS(1000UL / refreshRate));
       ESP_LOGI(TAG, "[%s] Starting", __func__);
       ESP_LOGI(TAG, "[%s] Refresh rate: %d Hz, task delay: %d ticks", __func__, refreshRate, taskDelay);
       for(uint32_t i = 25; i < 100; ++i)
       {
          tNeopixel pixel[PIXEL_COUNT];
          for(int px = 0; px < PIXEL_COUNT; ++px)
          {
              pixel[px].index = px;
              pixel[px].rgb = NP_RGB(i, i, i);
          }
          neopixel_SetPixel(neopixel, pixel, ARRAY_SIZE(pixel));
          vTaskDelay(taskDelay);
       }
        for(uint32_t i = 99; i > 24; --i)
       {
          tNeopixel pixel[PIXEL_COUNT];
          for(int px = 0; px < PIXEL_COUNT; ++px)
          {
              pixel[px].index = px;
              pixel[px].rgb = NP_RGB(i, i, i);
          }
          neopixel_SetPixel(neopixel, pixel, ARRAY_SIZE(pixel));
          vTaskDelay(taskDelay);
       }
       ESP_LOGI(TAG, "[%s] Finished", __func__);
       neopixel_Deinit(neopixel);
       return true;
    }

bool Main::chase()
{
   tNeopixelContext neopixel = neopixel_Init(PIXEL_COUNT, NEOPIXEL_CTRL_PIN);
   uint32_t refreshRate, taskDelay;

   if(NULL == neopixel)
   {
      ESP_LOGE(TAG, "[%s] Initialization failed\n", __func__);
      return false;
   }

   refreshRate = neopixel_GetRefreshRate(neopixel);
   taskDelay = MAX(1, pdMS_TO_TICKS(1000UL / refreshRate));
   ESP_LOGI(TAG, "[%s] Starting", __func__);
   ESP_LOGI(TAG, "[%s] Refresh rate: %d Hz, task delay: %d ticks", __func__, refreshRate, taskDelay);
   for(uint32_t i = 0; i < PIXEL_COUNT; ++i)
   {
      tNeopixel pixel[] =
      {
          { i % PIXEL_COUNT, NP_RGB(50, 50, 50) },
          { (i+PIXEL_COUNT-1) % PIXEL_COUNT, NP_RGB(0, 0, 0) },
      };
      neopixel_SetPixel(neopixel, pixel, ARRAY_SIZE(pixel));
      vTaskDelay(taskDelay);
   }
   ESP_LOGI(TAG, "[%s] Finished", __func__);
   neopixel_Deinit(neopixel);
   return true;
}

bool Main::test1(uint32_t iterations)
{
   tNeopixelContext neopixel = neopixel_Init(PIXEL_COUNT, NEOPIXEL_CTRL_PIN);
   tNeopixel pixel[] =
   {
       { 0, NP_RGB(50, 0,  0) }, /* green */
       { 0, NP_RGB(0,  50, 0) }, /* red */
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
         vTaskDelay(pdMS_TO_TICKS(1000));
      }
   }
   ESP_LOGI(TAG, "[%s] Finished", __func__);

   neopixel_Deinit(neopixel);
   return true;
}

bool Main::test2(uint32_t iterations)
{
   tNeopixelContext neopixel = neopixel_Init(PIXEL_COUNT, NEOPIXEL_CTRL_PIN);
   uint32_t refreshRate, taskDelay;

   if(NULL == neopixel)
   {
      ESP_LOGE(TAG, "[%s] Initialization failed\n", __func__);
      return false;
   }

   refreshRate = neopixel_GetRefreshRate(neopixel);
   taskDelay = MAX(1, pdMS_TO_TICKS(1000UL / refreshRate));
   ESP_LOGI(TAG, "[%s] Starting", __func__);
   ESP_LOGI(TAG, "[%s] Refresh rate: %d Hz, task delay: %d ticks", __func__, refreshRate, taskDelay);
   for(uint32_t i = 0; i < iterations; ++i)
   {
      tNeopixel pixel[] =
      {
          { 0, NP_RGB(i, 0, 0) },
          { 1, NP_RGB(0, i, 0) },
          { 2, NP_RGB(0, 0, i) },
          { 3, NP_RGB(i, i, 0) },
          { 4, NP_RGB(i, i, i) },
          { 5, NP_RGB(50-i, 0, 0) },
          { 6, NP_RGB(0, 50-i, 0) },
          { 7, NP_RGB(0, 0, 50-i) },
          { 8, NP_RGB(50-i, 50-i, 0) },
          { 9, NP_RGB(50-i, 50-i, 50-i) },
      };
      neopixel_SetPixel(neopixel, pixel, ARRAY_SIZE(pixel));
      vTaskDelay(taskDelay);
   }
   ESP_LOGI(TAG, "[%s] Finished", __func__);
   neopixel_Deinit(neopixel);
   return true;
}



} // namespace pinled
