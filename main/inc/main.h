#pragma once

/**
 * @file main.h
 * @author your name (you@domain.com)
 * @brief 
 * @version 0.1
 * @date 2025-12-21
 * 
 * @copyright Copyright (c) 2025
 * 
 * https://github.com/esp-cpp/espp/tree/e94ee66edcbfcb2706429d4f3e2f9c5692b7d7e8/components/qtpy
 * https://esp-cpp.github.io/espp/qtpy.html
 * https://github.com/adafruit/Adafruit-QT-Py-ESP32-Pico-PCB
 */

#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_log.h"

#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "neopixel.h"

#include "version.h" // for versioning

namespace ooe::pinled
{
    class Main
    {
    public:
        esp_err_t init();
        void run();

    private:
        void version();
        bool test1(uint32_t iterations);
        bool test2(uint32_t iterations);
    };
} // namespace pinled
