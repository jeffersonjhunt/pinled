FILE(GLOB_RECURSE app_sources src/*.cpp src/*.c)

idf_component_register(
        SRCS ${app_sources}
        INCLUDE_DIRS inc
        REQUIRES "driver" "esp_timer" "esp_driver_gptimer" "lamp_scan" "filament" "profiler" "lamp_map" "machine_config" "pinled_schema"
)

# Bring-up only, and destructive while it is on: this overwrites the device's
# stored configuration on every flash. See the Kconfig help. With it off — the
# default — flashing the app leaves the storage partition alone, which is both
# the normal behaviour and something worth having verified.
if(CONFIG_PINLED_SEED_FS)
    littlefs_create_partition_image(storage "${CMAKE_CURRENT_LIST_DIR}/../fs_seed/image" FLASH_IN_PROJECT)
endif()
