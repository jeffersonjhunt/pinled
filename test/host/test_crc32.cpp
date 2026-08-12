/**
 * @file test_crc32.cpp
 * @brief CRC-32/ISO-HDLC conformance and incremental use.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * The check vector is the point of this file. A reflected/non-reflected mix-up
 * produces a CRC that is perfectly self-consistent — it round-trips, it
 * detects corruption, every hand-written test passes — and is simply not
 * CRC-32. That only becomes visible the day something outside this codebase
 * has to agree with it.
 */

#include "harness.h"

#include "pinled_crc32.h"

#include <cstring>
#include <string>
#include <vector>

using namespace ooe::pinled;

TEST(crc32_canonical_check_value)
{
    // The check value every CRC-32/ISO-HDLC implementation must agree on.
    const char *s = "123456789";
    CHECK_EQ(crc32(s, 9), 0xCBF43926u);
}

TEST(crc32_empty_input_is_zero)
{
    CHECK_EQ(crc32("", 0), 0x00000000u);
    CHECK_EQ(crc32(nullptr, 0), 0x00000000u);
}

TEST(crc32_known_vectors)
{
    CHECK_EQ(crc32("a", 1), 0xE8B7BE43u);
    CHECK_EQ(crc32("abc", 3), 0x352441C2u);
    CHECK_EQ(crc32("The quick brown fox jumps over the lazy dog", 43), 0x414FA339u);
}

TEST(crc32_incremental_matches_one_shot)
{
    const std::string a = "the first part of a document, ";
    const std::string b = "and the second part which was written separately";
    const std::string whole = a + b;

    uint32_t r = crc32_begin();
    r = crc32_update(r, a.data(), a.size());
    r = crc32_update(r, b.data(), b.size());

    CHECK_EQ(crc32_end(r), crc32(whole.data(), whole.size()));
}

TEST(crc32_incremental_with_empty_chunks)
{
    const std::string s = "payload";
    uint32_t r = crc32_begin();
    r = crc32_update(r, nullptr, 0);
    r = crc32_update(r, s.data(), s.size());
    r = crc32_update(r, "", 0);
    CHECK_EQ(crc32_end(r), crc32(s.data(), s.size()));
}

TEST(crc32_detects_every_single_bit_flip)
{
    // The failure this exists to catch is a truncated or partially-written
    // document, but a single flipped bit is the strictest cheap proxy.
    std::vector<uint8_t> buf(64);
    for (size_t i = 0; i < buf.size(); ++i)
        buf[i] = static_cast<uint8_t>(i * 7 + 3);

    const uint32_t good = crc32(buf.data(), buf.size());

    int missed = 0;
    for (size_t byte = 0; byte < buf.size(); ++byte)
    {
        for (int bit = 0; bit < 8; ++bit)
        {
            buf[byte] ^= static_cast<uint8_t>(1u << bit);
            if (crc32(buf.data(), buf.size()) == good)
                ++missed;
            buf[byte] ^= static_cast<uint8_t>(1u << bit);
        }
    }
    CHECK_EQ(missed, 0);
}

TEST(crc32_is_order_sensitive)
{
    CHECK_NE(crc32("ab", 2), crc32("ba", 2));
}

TEST(crc32_distinguishes_trailing_zeros)
{
    // A CRC without the final XOR happily ignores appended zero bytes, which
    // is exactly the corruption a short write produces.
    const uint8_t a[3] = {1, 2, 3};
    const uint8_t b[5] = {1, 2, 3, 0, 0};
    CHECK_NE(crc32(a, sizeof a), crc32(b, sizeof b));
}

int main() { return ooe::test::run_all(); }
