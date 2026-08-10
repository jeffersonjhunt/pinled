# Schema layer: document framing, integrity, and the config vocabulary.
#
# Deliberately REQUIRES nothing. Every source here builds unmodified on the
# host as well as the target, which is what makes `test/host` possible without
# an ESP32 attached (FIRMWARE_PLAN.md §5.1 step 1). Adding an IDF dependency to
# this component breaks that, so if one is ever needed it belongs in
# machine_config instead.
#
# NOT registered here: src/pinled_resolve.cpp. It needs the nanopb header
# generated from proto/pinled.proto, and nanopb does not join the IDF build
# until step 4 wires up the store. Until then it is built and tested on the host
# only (test/host/CMakeLists.txt), which is the point of the `undeployed` tier —
# real code, fully verified, shipping nothing yet.
#
# Everything that IS registered must stay free of nanopb. filament.h and
# profiler.h now include headers from here, so anything reachable from them has
# to link in a firmware build that has no nanopb. That is why ChannelConfig
# lives in pinled_channel_config.h and the projection that builds one lives
# separately in pinled_resolve.h.

idf_component_register(
        SRCS "src/pinled_crc32.cpp"
             "src/pinled_doc_frame.cpp"
             "src/pinled_drive_class.cpp"
             "src/pinled_apply.cpp"
        INCLUDE_DIRS "include"
)
