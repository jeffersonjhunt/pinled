# Schema layer: document framing and integrity.
#
# Deliberately REQUIRES nothing. Every source here builds unmodified on the
# host as well as the target, which is what makes `test/host` possible without
# an ESP32 attached (FIRMWARE_PLAN.md §5.1 step 1). Adding an IDF dependency to
# this component breaks that, so if one is ever needed it belongs in
# machine_config instead.
#
# Step 2 adds the document/runtime model split (FR-CFG-14) here, and the
# nanopb-generated sources join it once nanopb is wired in.

idf_component_register(
        SRCS "src/pinled_crc32.cpp"
             "src/pinled_doc_frame.cpp"
        INCLUDE_DIRS "include"
)
