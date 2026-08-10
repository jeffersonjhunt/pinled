# Schema layer: document framing, integrity, and the config vocabulary.
#
# Deliberately REQUIRES nothing. Every source here builds unmodified on the
# host as well as the target, which is what makes `test/host` possible without
# an ESP32 attached (FIRMWARE_PLAN.md §5.1 step 1). Adding an IDF dependency to
# this component breaks that, so if one is ever needed it belongs in
# machine_config instead.
#
# NOT registered here: src/pinled_channel_config.cpp. It needs the nanopb
# header generated from proto/pinled.proto, and nanopb does not join the IDF
# build until step 4 wires up the store. Until then it is built and tested on
# the host only (test/host/CMakeLists.txt), which is the whole point of the
# `undeployed` tier — real code, fully verified, shipping nothing yet.
#
# pinled_drive_class.cpp is registered, and must stay free of nanopb: profiler.h
# includes pinled_drive_class.h, so anything reachable from it has to link in a
# firmware build that has no nanopb.

idf_component_register(
        SRCS "src/pinled_crc32.cpp"
             "src/pinled_doc_frame.cpp"
             "src/pinled_drive_class.cpp"
        INCLUDE_DIRS "include"
)
