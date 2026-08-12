idf_component_register(
        SRCS "net.cpp"
        INCLUDE_DIRS "include"
        REQUIRES "esp_wifi" "esp_netif" "esp_event" "espressif__mdns"
)
