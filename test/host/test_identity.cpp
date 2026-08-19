/**
 * @file test_identity.cpp
 * @brief The author-handle rule: what gets into NVS and what does not.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * The handle is stamped into exported profiles (FR-REG-1), so the rule's job
 * is to refuse anything that would make two attributions look identical or
 * corrupt a log line — and nothing more. Ownership and uniqueness are the
 * cloud's problem, deliberately.
 */

#include "harness.h"
#include "pinled_identity.h"

#include <string>

using namespace ooe::pinled;

TEST(ordinary_handles_pass)
{
    CHECK(author_handle_valid("jhunt"));
    CHECK(author_handle_valid("Jefferson J. Hunt"));
    CHECK(author_handle_valid("a")); // one byte is a handle
    CHECK(author_handle_valid("x-2_49!"));
}

TEST(the_boundary_is_31_bytes_inclusive)
{
    const std::string at_max(kAuthorHandleMax, 'h');
    CHECK(author_handle_valid(at_max.c_str()));
    const std::string past_max(kAuthorHandleMax + 1, 'h');
    CHECK(!author_handle_valid(past_max.c_str()));
}

TEST(empty_and_null_are_not_handles)
{
    CHECK(!author_handle_valid(""));
    CHECK(!author_handle_valid(nullptr));
}

TEST(invisible_edges_are_refused)
{
    // " jhunt" and "jhunt" must never be two different attributions that
    // print identically.
    CHECK(!author_handle_valid(" jhunt"));
    CHECK(!author_handle_valid("jhunt "));
    CHECK(author_handle_valid("j hunt")); // interior space is a name
    CHECK(!author_handle_valid(" "));     // leading AND trailing, and empty of content
}

TEST(control_bytes_and_del_are_refused)
{
    CHECK(!author_handle_valid("j\nhunt"));
    CHECK(!author_handle_valid("j\thunt"));
    CHECK(!author_handle_valid("jhunt\x7f"));
    CHECK(!author_handle_valid("\x01"));
}

TEST(utf8_passes_through)
{
    // "Björn" as UTF-8: bytes >= 0x80 are the browser's business, not ours.
    CHECK(author_handle_valid("Bj\xc3\xb6rn"));
}

int main() { return ooe::test::run_all(); }
