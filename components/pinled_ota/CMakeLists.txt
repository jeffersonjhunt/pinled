# The OTA staging driver half. The rules — transition legality, the
# confirm/expiry race, wrap-safe deadlines — live in pinled_schema
# (pinled_ota_state.h) and are tested on the host; this component owns the
# flash handle, the SHA-256, the timers, and nothing worth asserting.
#
# app_update supplies esp_ota_*, mbedtls the digest that backstops the
# browser's published-hash check (FR-OTA-3).
idf_component_register(
        SRCS "ota_manager.cpp"
        INCLUDE_DIRS "include"
        REQUIRES "app_update" "esp_timer" "mbedtls" "pinled_schema"
)
