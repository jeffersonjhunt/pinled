FILE(GLOB_RECURSE app_sources src/*.cpp src/*.c)

idf_component_register(
        SRCS ${app_sources}
        INCLUDE_DIRS inc
        REQUIRES "driver" "esp_timer" "esp_driver_gptimer" "lamp_scan" "filament" "profiler" "lamp_map" "machine_config"
)
