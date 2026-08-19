# The status pixel's driver half. The pattern generation it calls lives in
# pinled_schema, which has no IDF dependency and is tested on the host; this
# component is deliberately the part with nothing worth asserting in it.
#
# RMT rather than the zorxx/neopixel component the playfield string uses: that
# one is an I2S driver, and the ESP32-S3 has two I2S controllers against four
# RMT TX channels. See the note in indicator.cpp.
idf_component_register(
        SRCS "indicator.cpp"
        INCLUDE_DIRS "include"
        REQUIRES "driver" "esp_driver_rmt" "esp_timer" "pinled_schema"
)
