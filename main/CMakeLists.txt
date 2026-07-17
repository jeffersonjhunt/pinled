FILE(GLOB_RECURSE app_sources src/*.cpp src/*.c)

idf_component_register(
        SRCS ${app_sources}
        INCLUDE_DIRS inc
        REQUIRES "driver" "lamp_scan" "filament" "profiler" "lamp_map" "machine_config"
)
