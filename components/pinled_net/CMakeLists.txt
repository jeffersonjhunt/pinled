idf_component_register(
        SRCS "net.cpp"
             "credentials.cpp"
             "dns_hijack.cpp"
             "rescue.cpp"
        INCLUDE_DIRS "include"
        REQUIRES "esp_wifi" "esp_netif" "esp_event" "espressif__mdns" "nvs_flash" "lwip" "driver"
)
