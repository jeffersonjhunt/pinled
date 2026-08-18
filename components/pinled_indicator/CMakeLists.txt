# The status pixel's driver half. The pattern generation it calls lives in
# pinled_schema, which has no IDF dependency and is tested on the host; this
# component is deliberately the part with nothing worth asserting in it.
idf_component_register(
        SRCS "indicator.cpp"
        INCLUDE_DIRS "include"
        REQUIRES "driver" "esp_timer" "pinled_schema"
)
