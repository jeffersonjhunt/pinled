# One boot-time setting in NVS (FR-REG-1, FR-CFG-7). The validity rule is in
# pinled_schema where the host tests it; this component only persists what
# already passed.
idf_component_register(
        SRCS "author_store.cpp"
        INCLUDE_DIRS "include"
        REQUIRES "nvs_flash" "pinled_schema"
)
