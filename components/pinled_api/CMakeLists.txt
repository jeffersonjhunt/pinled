idf_component_register(
        SRCS "api.cpp"
        INCLUDE_DIRS "include"
        REQUIRES "esp_http_server" "esp_app_format" "esp_timer"
                 "machine_config" "pinled_net" "pinled_schema" "pinled_config" "nanopb"
)
