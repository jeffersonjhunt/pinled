/**
 * @file test_doc_frame.cpp
 * @brief Stored-document framing: round-trip, and every way it should refuse.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * The happy path is one test. The rest of this file is the failure modes,
 * because the reason the frame exists at all is that LittleFS guarantees
 * filesystem consistency and not that the last write landed (FR-CFG-15) — so
 * the interesting behaviour is entirely in what it rejects.
 */

#include "harness.h"

#include "pinled_crc32.h"
#include "pinled_doc_frame.h"

#include <cstring>
#include <vector>

using namespace ooe::pinled;

namespace
{
    /// A stand-in for an encoded protobuf payload.
    std::vector<uint8_t> sample_payload(size_t n = 40)
    {
        std::vector<uint8_t> v(n);
        for (size_t i = 0; i < n; ++i)
            v[i] = static_cast<uint8_t>(0x40 + (i % 191));
        return v;
    }

    /// Frame a payload, asserting the write succeeded.
    std::vector<uint8_t> framed(const std::vector<uint8_t> &payload,
                                DocKind kind = DocKind::MachineProfile)
    {
        std::vector<uint8_t> out(doc_frame_size(payload.size()));
        size_t written = 0;
        const DocStatus s = doc_frame_write(out.data(), out.size(), kind,
                                            payload.data(), payload.size(), &written);
        CHECK_EQ(s, DocStatus::Ok);
        CHECK_EQ(written, out.size());
        return out;
    }
} // namespace

TEST(frame_round_trips)
{
    const auto payload = sample_payload();
    const auto buf = framed(payload, DocKind::InstallConfig);

    DocKind kind = DocKind::Unknown;
    const uint8_t *body = nullptr;
    size_t body_len = 0;

    CHECK_EQ(doc_frame_read(buf.data(), buf.size(), &kind, &body, &body_len), DocStatus::Ok);
    CHECK_EQ(kind, DocKind::InstallConfig);
    CHECK_EQ(body_len, payload.size());
    CHECK(body != nullptr);
    CHECK_EQ(std::memcmp(body, payload.data(), payload.size()), 0);
}

TEST(frame_header_is_sixteen_bytes)
{
    // On-disk constant. If this changes, every stored document from every
    // previous build becomes unreadable, so it is asserted rather than assumed.
    CHECK_EQ(kDocHeaderSize, static_cast<size_t>(16));
    CHECK_EQ(doc_frame_size(0), static_cast<size_t>(16));
    CHECK_EQ(doc_frame_size(100), static_cast<size_t>(116));
}

TEST(frame_starts_with_magic_and_version)
{
    const auto buf = framed(sample_payload());
    CHECK_EQ(buf[0], static_cast<uint8_t>('P'));
    CHECK_EQ(buf[1], static_cast<uint8_t>('L'));
    CHECK_EQ(buf[2], static_cast<uint8_t>('D'));
    CHECK_EQ(buf[3], static_cast<uint8_t>('1'));
    CHECK_EQ(buf[4], static_cast<uint8_t>(1));
}

TEST(frame_accepts_empty_payload)
{
    // A profile with no lamps yet is legitimate — a machine mid-commissioning.
    std::vector<uint8_t> out(doc_frame_size(0));
    size_t written = 0;
    CHECK_EQ(doc_frame_write(out.data(), out.size(), DocKind::MachineProfile,
                             nullptr, 0, &written),
             DocStatus::Ok);
    CHECK_EQ(written, static_cast<size_t>(16));

    size_t body_len = 99;
    CHECK_EQ(doc_frame_read(out.data(), out.size(), nullptr, nullptr, &body_len), DocStatus::Ok);
    CHECK_EQ(body_len, static_cast<size_t>(0));
}

TEST(frame_write_refuses_a_short_buffer)
{
    const auto payload = sample_payload();
    std::vector<uint8_t> out(doc_frame_size(payload.size()) - 1);
    CHECK_EQ(doc_frame_write(out.data(), out.size(), DocKind::Version,
                             payload.data(), payload.size(), nullptr),
             DocStatus::ShortBuffer);
}

TEST(frame_write_refuses_null_payload_with_nonzero_length)
{
    std::vector<uint8_t> out(64);
    CHECK_EQ(doc_frame_write(out.data(), out.size(), DocKind::Version, nullptr, 8, nullptr),
             DocStatus::ShortBuffer);
}

TEST(frame_read_rejects_input_shorter_than_a_header)
{
    const auto buf = framed(sample_payload());
    for (size_t n = 0; n < kDocHeaderSize; ++n)
        CHECK_EQ(doc_frame_read(buf.data(), n, nullptr, nullptr, nullptr),
                 DocStatus::ShortBuffer);

    CHECK_EQ(doc_frame_read(nullptr, 64, nullptr, nullptr, nullptr), DocStatus::ShortBuffer);
}

TEST(frame_read_rejects_bad_magic)
{
    auto buf = framed(sample_payload());
    buf[2] = 'X';
    CHECK_EQ(doc_frame_read(buf.data(), buf.size(), nullptr, nullptr, nullptr),
             DocStatus::BadMagic);
}

TEST(frame_read_rejects_unknown_framing_version)
{
    auto buf = framed(sample_payload());
    buf[4] = 2;
    CHECK_EQ(doc_frame_read(buf.data(), buf.size(), nullptr, nullptr, nullptr),
             DocStatus::BadVersion);
}

TEST(frame_read_rejects_truncation)
{
    // The failure the CRC exists for: the header says N bytes, the file holds
    // fewer because power went away mid-write.
    const auto payload = sample_payload();
    const auto buf = framed(payload);

    for (size_t cut = 1; cut <= payload.size(); ++cut)
    {
        const size_t len = buf.size() - cut;
        CHECK_EQ(doc_frame_read(buf.data(), len, nullptr, nullptr, nullptr),
                 DocStatus::BadLength);
    }
}

TEST(frame_read_rejects_an_overlong_declared_length)
{
    // A corrupt length field is the one value that could walk the checksum off
    // the end of the buffer, so it must be refused before the CRC runs.
    auto buf = framed(sample_payload());
    buf[8] = 0xFF;
    buf[9] = 0xFF;
    buf[10] = 0xFF;
    buf[11] = 0x7F;
    CHECK_EQ(doc_frame_read(buf.data(), buf.size(), nullptr, nullptr, nullptr),
             DocStatus::BadLength);
}

TEST(frame_read_rejects_a_corrupt_payload)
{
    const auto payload = sample_payload();
    auto buf = framed(payload);

    for (size_t i = 0; i < payload.size(); ++i)
    {
        buf[kDocHeaderSize + i] ^= 0x01;
        CHECK_EQ(doc_frame_read(buf.data(), buf.size(), nullptr, nullptr, nullptr),
                 DocStatus::BadCrc);
        buf[kDocHeaderSize + i] ^= 0x01;
    }
}

TEST(frame_read_rejects_a_corrupt_crc_field)
{
    auto buf = framed(sample_payload());
    buf[12] ^= 0x80;
    CHECK_EQ(doc_frame_read(buf.data(), buf.size(), nullptr, nullptr, nullptr),
             DocStatus::BadCrc);
}

TEST(frame_kind_survives_the_round_trip)
{
    const DocKind kinds[] = {DocKind::Unknown, DocKind::InstallConfig,
                             DocKind::MachineProfile, DocKind::Version};
    for (DocKind k : kinds)
    {
        const auto buf = framed(sample_payload(8), k);
        DocKind got = DocKind::Unknown;
        CHECK_EQ(doc_frame_read(buf.data(), buf.size(), &got, nullptr, nullptr), DocStatus::Ok);
        CHECK_EQ(got, k);
    }
}

TEST(frame_read_tolerates_trailing_bytes)
{
    // Reading is length-driven, not file-length-driven, so a file that is
    // longer than its document still reads. This is deliberate: it is what
    // lets a short document be rewritten over a longer one without the store
    // having to truncate first.
    const auto payload = sample_payload();
    auto buf = framed(payload);
    buf.insert(buf.end(), 32, 0xAA);

    size_t body_len = 0;
    CHECK_EQ(doc_frame_read(buf.data(), buf.size(), nullptr, nullptr, &body_len), DocStatus::Ok);
    CHECK_EQ(body_len, payload.size());
}

TEST(frame_read_accepts_null_out_params)
{
    const auto buf = framed(sample_payload());
    CHECK_EQ(doc_frame_read(buf.data(), buf.size(), nullptr, nullptr, nullptr), DocStatus::Ok);
}

TEST(doc_status_str_covers_every_value)
{
    const DocStatus all[] = {DocStatus::Ok, DocStatus::ShortBuffer, DocStatus::BadMagic,
                             DocStatus::BadVersion, DocStatus::BadLength, DocStatus::BadCrc};
    for (DocStatus s : all)
    {
        const char *t = doc_status_str(s);
        CHECK(t != nullptr);
        CHECK(std::strcmp(t, "unknown") != 0);
    }
}

int main() { return ooe::test::run_all(); }
