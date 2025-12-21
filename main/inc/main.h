#pragma once

#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
    };
} // namespace pinled
