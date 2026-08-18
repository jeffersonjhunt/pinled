# Schema layer: document framing, integrity, and the config vocabulary.
#
# Deliberately REQUIRES nothing. Every source here builds unmodified on the
# host as well as the target, which is what makes `test/host` possible without
# an ESP32 attached (FIRMWARE_PLAN.md §5.1 step 1). Adding an IDF dependency to
# this component breaks that, so if one is ever needed it belongs in
# machine_config instead.
#
# Everything registered here must stay free of nanopb, and as of step 4 that is
# enforced by construction rather than asked for in a comment: the projection
# that needs the generated header moved to its own component, `pinled_config`.
# filament.h and profiler.h include headers from here, so anything reachable
# from them has to link in a build that has no nanopb.
#
# pinled_doc_file.cpp belongs here despite doing file I/O: it is plain <cstdio>,
# because LittleFS is reached through the IDF's VFS layer and `fopen` is the
# real interface on the target. That keeps it host-testable against a temporary
# directory, with genuinely corrupt files rather than simulated ones.

idf_component_register(
        SRCS "src/pinled_crc32.cpp"
             "src/pinled_color_order.cpp"
             "src/pinled_doc_frame.cpp"
             "src/pinled_doc_file.cpp"
             "src/pinled_live.cpp"
             "src/pinled_drive_class.cpp"
             "src/pinled_apply.cpp"
             "src/pinled_profiling.cpp"
             "src/pinled_indicator.cpp"
        INCLUDE_DIRS "include"
)
