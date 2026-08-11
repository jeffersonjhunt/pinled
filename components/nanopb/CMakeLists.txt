# nanopb for the firmware build: the C runtime, plus the sources generated from
# proto/pinled.proto.
#
# WHY THIS COMPONENT IS A WRAPPER AND NOT A COPY
# ----------------------------------------------
# nanopb is not in the ESP component registry, so it has to come from somewhere.
# It comes from a checkout on the build host — the same one test/host already
# uses — rather than being vendored into this repo.
#
# The tempting middle road, vendoring the runtime but still generating at build
# time, buys nothing: the generator stays machine-local either way, so the
# firmware still only builds where the toolchain is. That would add third-party
# source to the repo for no gained property. See docs/FIRMWARE_PLAN.md §5.1
# step 4.
#
# The consequence is explicit and intended: **this firmware builds on a machine
# with the nanopb toolchain installed**, exactly like the schema tests. If that
# ever needs to change, vendor the runtime AND commit the generated sources
# together, with a regeneration check like test/host/fixtures/regenerate.py has
# — half the change is worse than neither half.
#
# NOTHING GENERATED IS COMMITTED
# ------------------------------
# pinled.pb.c/h are produced into the build directory on every configure, from
# proto/pinled.proto and proto/pinled.options. That keeps one schema authority
# (FR-CFG-13) by construction rather than by discipline: there is no second copy
# that could drift.

set(PINLED_NANOPB_DIR "$ENV{HOME}/.local/src/nanopb"
        CACHE PATH "Path to a nanopb checkout supplying the C runtime")

find_program(PINLED_NANOPB_GENERATOR nanopb_generator
        HINTS "$ENV{HOME}/.venvs/pinled-tools/bin" "${PINLED_NANOPB_DIR}/generator")

# Fail at configure time with the fix in the message. A missing generator that
# surfaces as a missing header three minutes into a compile is a worse day.
if(NOT EXISTS "${PINLED_NANOPB_DIR}/pb.h")
    message(FATAL_ERROR
            "nanopb runtime not found at ${PINLED_NANOPB_DIR}\n"
            "  git clone --branch nanopb-0.4.9 https://github.com/nanopb/nanopb ~/.local/src/nanopb\n"
            "  or configure with -DPINLED_NANOPB_DIR=/path/to/nanopb")
endif()
if(NOT PINLED_NANOPB_GENERATOR)
    message(FATAL_ERROR
            "nanopb_generator not found (looked in ~/.venvs/pinled-tools/bin and "
            "${PINLED_NANOPB_DIR}/generator)\n"
            "  python3 -m venv ~/.venvs/pinled-tools && ~/.venvs/pinled-tools/bin/pip install nanopb\n"
            "  and install protoc (apt install protobuf-compiler)")
endif()

set(PROTO_DIR "${CMAKE_CURRENT_LIST_DIR}/../../proto")
set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${GEN_DIR}")

idf_component_register(
        SRCS "${GEN_DIR}/pinled.pb.c"
             "${PINLED_NANOPB_DIR}/pb_common.c"
             "${PINLED_NANOPB_DIR}/pb_encode.c"
             "${PINLED_NANOPB_DIR}/pb_decode.c"
        INCLUDE_DIRS "${PINLED_NANOPB_DIR}" "${GEN_DIR}")

# Everything below is guarded because IDF reads every component's CMakeLists
# TWICE: once in script mode to work out the dependency graph, where
# add_custom_command and friends do not exist and ${COMPONENT_LIB} is not yet a
# target. Only the second pass builds anything.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    # pinled.options is as much an input as the .proto: it decides the static
    # footprint of every generated struct, so a change there is a schema change.
    add_custom_command(
            OUTPUT "${GEN_DIR}/pinled.pb.c" "${GEN_DIR}/pinled.pb.h"
            COMMAND "${PINLED_NANOPB_GENERATOR}"
                    -I "${PROTO_DIR}" -D "${GEN_DIR}" "${PROTO_DIR}/pinled.proto"
            DEPENDS "${PROTO_DIR}/pinled.proto" "${PROTO_DIR}/pinled.options"
            COMMENT "nanopb: generating pinled.pb.c/h"
            VERBATIM)

    # The generated sources do not exist when CMake first reads this file.
    add_custom_target(pinled_pb_generate
            DEPENDS "${GEN_DIR}/pinled.pb.c" "${GEN_DIR}/pinled.pb.h")
    add_dependencies(${COMPONENT_LIB} pinled_pb_generate)

    # Third-party and machine-generated code, built without our warning set.
    # Turning an upstream nanopb bump into a build break is not a safety
    # property, it is an obstacle to taking a fix.
    set_source_files_properties(
            "${GEN_DIR}/pinled.pb.c"
            "${PINLED_NANOPB_DIR}/pb_common.c"
            "${PINLED_NANOPB_DIR}/pb_encode.c"
            "${PINLED_NANOPB_DIR}/pb_decode.c"
            TARGET_DIRECTORY ${COMPONENT_LIB}
            PROPERTIES COMPILE_OPTIONS "-w")
endif()
