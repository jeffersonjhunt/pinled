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
project(pinled_host_tests CXX)

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

add_library(pinled_schema STATIC
        "${SCHEMA_DIR}/src/pinled_crc32.cpp"
        "${SCHEMA_DIR}/src/pinled_doc_frame.cpp")
target_include_directories(pinled_schema PUBLIC "${SCHEMA_DIR}/include")

enable_testing()

function(pinled_add_test name)
    add_executable(${name} "${name}.cpp")
    target_link_libraries(${name} PRIVATE pinled_schema)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    add_test(NAME ${name} COMMAND ${name})
endfunction()

pinled_add_test(test_crc32)
pinled_add_test(test_doc_frame)

# --- nanopb-dependent tests --------------------------------------------------
#
# Schema round-trips (document -> protobuf -> document) need generated sources,
# which need protoc plus the nanopb generator. Point PINLED_NANOPB_DIR at a
# nanopb checkout to enable them; see test/host/README.md.

set(PINLED_NANOPB_DIR "" CACHE PATH "Path to a nanopb checkout (enables schema round-trip tests)")

if(PINLED_NANOPB_DIR AND EXISTS "${PINLED_NANOPB_DIR}/pb.h")
    message(STATUS "pinled: nanopb found at ${PINLED_NANOPB_DIR} — schema tests enabled")
    # Wired up in step 2, when the document/runtime models exist to round-trip.
else()
    message(STATUS "pinled: nanopb not configured — schema round-trip tests skipped "
                   "(set -DPINLED_NANOPB_DIR=/path/to/nanopb)")
endif()
