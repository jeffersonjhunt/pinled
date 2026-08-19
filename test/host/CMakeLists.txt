# Host test build — no ESP-IDF, no target, no board.
#
#   cmake -S test/host -B build-host && cmake --build build-host && ctest --test-dir build-host
#
# Everything here builds against components/ sources directly. That is only
# possible because those sources have no IDF dependency, which is a property
# worth defending: the moment a schema source includes esp_log.h it stops being
# testable off-target and this file stops working.
#
# The nanopb-dependent tests are guarded by PINLED_WITH_NANOPB and are skipped
# entirely when nanopb is absent, so a machine without protoc still gets a
# green run for everything that does not need it (FIRMWARE_PLAN.md §5.1 step 1).

cmake_minimum_required(VERSION 3.16)
# C as well as C++: nanopb's runtime and its generated sources are C.
project(pinled_host_tests C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)

# Sanitizers by default: these are buffer-parsing routines fed deliberately
# corrupt input, which is precisely what ASan and UBSan are for. A read that
# walks off the end of a truncated document is the bug class this whole layer
# exists to prevent, and a passing test that quietly reads out of bounds would
# be worse than no test.
option(PINLED_SANITIZE "Build host tests with ASan/UBSan" ON)
if(PINLED_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

set(PINLED_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(SCHEMA_DIR "${PINLED_ROOT}/components/pinled_schema")
set(CONFIG_DIR "${PINLED_ROOT}/components/pinled_config")

add_library(pinled_schema STATIC
        "${SCHEMA_DIR}/src/pinled_crc32.cpp"
        "${SCHEMA_DIR}/src/pinled_color_order.cpp"
        "${SCHEMA_DIR}/src/pinled_doc_frame.cpp"
        "${SCHEMA_DIR}/src/pinled_doc_file.cpp"
        "${SCHEMA_DIR}/src/pinled_live.cpp"
        "${SCHEMA_DIR}/src/pinled_drive_class.cpp"
        "${SCHEMA_DIR}/src/pinled_apply.cpp"
        "${SCHEMA_DIR}/src/pinled_profiling.cpp"
        "${SCHEMA_DIR}/src/pinled_indicator.cpp"
        "${SCHEMA_DIR}/src/pinled_ota_state.cpp")
target_include_directories(pinled_schema PUBLIC "${SCHEMA_DIR}/include")

enable_testing()

# Tests that touch real files use a RELATIVE scratch directory and create it
# themselves, so the working directory is all they need. An absolute path from
# here was the first attempt and it failed everything: kMaxDocPath is 96 bytes
# and a build directory nested a few levels deep exceeds that unaided.
function(pinled_add_test name)
    add_executable(${name} "${name}.cpp")
    target_link_libraries(${name} PRIVATE pinled_schema)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

pinled_add_test(test_crc32)
pinled_add_test(test_doc_frame)
pinled_add_test(test_apply)
pinled_add_test(test_doc_file)
pinled_add_test(test_color_order)
pinled_add_test(test_live)
target_link_libraries(test_live PRIVATE pthread)
pinled_add_test(test_profiling)
pinled_add_test(test_indicator)
pinled_add_test(test_ota_state)

# --- nanopb-dependent tests --------------------------------------------------
#
# Schema round-trips (document -> protobuf -> document) need generated sources,
# which need protoc plus the nanopb generator. Point PINLED_NANOPB_DIR at a
# nanopb checkout to enable them; see test/host/README.md.

set(PINLED_NANOPB_DIR "" CACHE PATH "Path to a nanopb checkout (enables schema round-trip tests)")
find_program(PINLED_NANOPB_GENERATOR nanopb_generator
        HINTS "$ENV{HOME}/.venvs/pinled-tools/bin" "${PINLED_NANOPB_DIR}/generator")

if(PINLED_NANOPB_DIR AND EXISTS "${PINLED_NANOPB_DIR}/pb.h" AND PINLED_NANOPB_GENERATOR)
    message(STATUS "pinled: nanopb at ${PINLED_NANOPB_DIR} — schema round-trip tests enabled")

    set(PROTO_DIR "${PINLED_ROOT}/proto")
    set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")
    file(MAKE_DIRECTORY "${GEN_DIR}")

    # Regenerates whenever the schema OR its nanopb bounds change. Both are
    # inputs: pinled.options decides the static footprint, so a change there is
    # every bit as much a schema change as editing the .proto.
    add_custom_command(
            OUTPUT "${GEN_DIR}/pinled.pb.c" "${GEN_DIR}/pinled.pb.h"
            COMMAND "${PINLED_NANOPB_GENERATOR}"
                    -I "${PROTO_DIR}" -D "${GEN_DIR}" "${PROTO_DIR}/pinled.proto"
            DEPENDS "${PROTO_DIR}/pinled.proto" "${PROTO_DIR}/pinled.options"
            COMMENT "nanopb: generating pinled.pb.c/h"
            VERBATIM)

    # nanopb's runtime and the generated sources are third-party output, so
    # they are compiled without our warning set. -Werror on code we do not
    # own would make an upstream bump a build break rather than a decision.
    add_library(pinled_pb STATIC
            "${GEN_DIR}/pinled.pb.c"
            "${PINLED_NANOPB_DIR}/pb_encode.c"
            "${PINLED_NANOPB_DIR}/pb_decode.c"
            "${PINLED_NANOPB_DIR}/pb_common.c")
    target_include_directories(pinled_pb SYSTEM PUBLIC "${PINLED_NANOPB_DIR}" "${GEN_DIR}")
    target_compile_options(pinled_pb PRIVATE -w)

    # The projections need the generated document model, so they live in their
    # own component rather than in the dependency-free part of pinled_schema —
    # a split the firmware build now mirrors exactly (components/pinled_config).
    add_library(pinled_config STATIC "${CONFIG_DIR}/src/pinled_resolve.cpp")
    target_include_directories(pinled_config PUBLIC "${CONFIG_DIR}/include")
    target_link_libraries(pinled_config PUBLIC pinled_schema pinled_pb)

    foreach(t test_schema_roundtrip test_resolve test_install)
        pinled_add_test(${t})
        target_link_libraries(${t} PRIVATE pinled_config)
        target_compile_definitions(${t} PRIVATE
                PINLED_FIXTURE_DIR="${CMAKE_CURRENT_LIST_DIR}/fixtures")
    endforeach()
else()
    message(STATUS "pinled: nanopb not configured — schema round-trip tests skipped "
                   "(set -DPINLED_NANOPB_DIR=/path/to/nanopb)")
endif()
