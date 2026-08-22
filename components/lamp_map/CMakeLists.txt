idf_component_register(
        SRCS "lamp_map.cpp"
        INCLUDE_DIRS "include"
        REQUIRES "driver" "pinled_schema"
        PRIV_REQUIRES "esp32-idf-ws2812b"
)
