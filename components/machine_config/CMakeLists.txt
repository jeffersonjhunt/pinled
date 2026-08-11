idf_component_register(
        SRCS "machine_config.cpp"
        INCLUDE_DIRS "include"
        REQUIRES "nvs_flash" "joltwallet__littlefs"
                 "pinled_schema" "pinled_config" "nanopb"
)
