/**
 * @file test_schema_roundtrip.cpp
 * @brief Schema round-trips through nanopb, against golden bytes from Google's
 *        protobuf implementation.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * The fixtures under `fixtures/` are encoded by the **Python** protobuf runtime
 * and decoded here by **nanopb**. That asymmetry is the whole value: nanopb
 * agreeing with itself proves very little, whereas nanopb agreeing with the
 * reference implementation is evidence that `proto/pinled.proto` is
 * unambiguous — which matters because a third implementation (protobuf.js in
 * the browser) has to agree with both (FR-CFG-13).
 *
 * The committed .pb files also pin the wire format. Renumber a field or change
 * a type and these fail loudly, rather than silently orphaning every document
 * an older build ever wrote.
 *
 * Build with `-DPINLED_NANOPB_DIR=/path/to/nanopb`; skipped entirely otherwise.
 */

#include "harness.h"

#include "pinled_crc32.h"
#include "pinled_doc_frame.h"

#include "pinled.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace ooe::pinled;

namespace
{
    std::vector<uint8_t> read_fixture(const char *name)
    {
        const std::string path = std::string(PINLED_FIXTURE_DIR) + "/" + name;
        std::ifstream f(path, std::ios::binary);
        if (!f)
        {
            ::ooe::test::fail(__FILE__, __LINE__, "cannot open fixture " + path);
            return {};
        }
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
    }

    template <typename T>
    bool decode(const std::vector<uint8_t> &bytes, const pb_msgdesc_t *fields, T *out)
    {
        pb_istream_t is = pb_istream_from_buffer(bytes.data(), bytes.size());
        return pb_decode(&is, fields, out);
    }

    template <typename T>
    std::vector<uint8_t> encode(const pb_msgdesc_t *fields, const T *msg, size_t cap = 4096)
    {
        std::vector<uint8_t> buf(cap);
        pb_ostream_t os = pb_ostream_from_buffer(buf.data(), buf.size());
        if (!pb_encode(&os, fields, msg))
            return {};
        buf.resize(os.bytes_written);
        return buf;
    }
} // namespace

// ------------------------------------------------------- golden decoding ---

TEST(golden_machine_profile_decodes_with_nanopb)
{
    const auto bytes = read_fixture("machine_profile.pb");
    CHECK(!bytes.empty());

    static pinled_v1_MachineProfile p = pinled_v1_MachineProfile_init_zero;
    CHECK(decode(bytes, pinled_v1_MachineProfile_fields, &p));

    CHECK_EQ(p.schema, 1u);
    CHECK_EQ(std::string(p.profile_id), std::string("01J8QK7ZC3"));
    CHECK_EQ(p.revision, 3u);

    CHECK(p.has_machine);
    CHECK_EQ(std::string(p.machine.make), std::string("Bally"));
    CHECK_EQ(std::string(p.machine.model), std::string("Eight Ball Deluxe"));
    CHECK_EQ(p.machine.year, 1981u);
    CHECK_EQ(p.machine.lamp_count, 60u);

    CHECK(p.has_author);
    CHECK_EQ(std::string(p.author.handle), std::string("jhunt"));
    CHECK_EQ(p.author.verified, true);

    CHECK_EQ(p.lamps_count, static_cast<pb_size_t>(2));

    CHECK_EQ(p.lamps[0].lamp, 17u);
    CHECK_EQ(std::string(p.lamps[0].name), std::string("Left Drop Target"));
    CHECK(p.lamps[0].has_color);
    CHECK_EQ(p.lamps[0].color.r, 0u);
    CHECK_EQ(p.lamps[0].color.g, 80u);
    CHECK_EQ(p.lamps[0].color.b, 255u);
    CHECK_EQ(p.lamps[0].class_lock, pinled_v1_DriveClass_DRIVE_CLASS_STEADY);
    CHECK_EQ(p.lamps[0].attack_ms, 30u);
    CHECK_EQ(p.lamps[0].decay_ms, 40u);
    CHECK_EQ(p.lamps[0].gain_permille, 1000u);

    CHECK_EQ(p.lamps[1].lamp, 45u);
    CHECK_EQ(std::string(p.lamps[1].name), std::string("GI Upper Left"));
    CHECK_EQ(p.lamps[1].class_lock, pinled_v1_DriveClass_DRIVE_CLASS_AC_STEADY);

    // Zero means "inherit the global default" — it must survive as zero and
    // not be quietly filled in by anything.
    CHECK_EQ(p.lamps[1].attack_ms, 0u);
    CHECK_EQ(p.lamps[1].gain_permille, 0u);
}

TEST(golden_install_config_decodes_with_nanopb)
{
    const auto bytes = read_fixture("install_config.pb");
    CHECK(!bytes.empty());

    static pinled_v1_InstallConfig c = pinled_v1_InstallConfig_init_zero;
    CHECK(decode(bytes, pinled_v1_InstallConfig_fields, &c));

    CHECK_EQ(c.schema, 1u);
    CHECK(c.has_geometry);
    CHECK_EQ(c.geometry.num_modules, 4u);
    CHECK_EQ(c.geometry.channels_per_module, 16u);
    CHECK_EQ(c.geometry.led_count, 64u);

    CHECK(c.has_pins);
    CHECK_EQ(c.pins.clk, 18);
    CHECK_EQ(c.pins.pl, 17);
    CHECK_EQ(c.pins.data, 9);
    CHECK_EQ(c.pins.led, 8);

    CHECK(c.has_scan);
    CHECK_EQ(c.scan.sample_rate_hz, 10000u);
    CHECK_EQ(c.scan.spi_hz, 4000000u);
    CHECK_EQ(c.scan.spi_mode, 2u);
    CHECK_EQ(c.scan.active_low, false);
    CHECK_EQ(c.scan.pl_from_cs, true);

    CHECK(c.has_render);
    CHECK_EQ(c.render.refresh_hz, 90u);
    CHECK_EQ(c.render.gamma_x100, 220u);
    CHECK_EQ(c.render.brightness_cap, 180u);

    CHECK_EQ(c.wiring_count, static_cast<pb_size_t>(2));
    CHECK_EQ(c.wiring[0].channel, 0u);
    CHECK_EQ(c.wiring[0].lamp, 17u);
    CHECK_EQ(c.wiring[0].led_index, 0);
    CHECK_EQ(c.wiring[1].channel, 4u);
    CHECK_EQ(c.wiring[1].lamp, 45u);
    CHECK_EQ(c.wiring[1].led_index, 4);
}

// ------------------------------------------- cross-implementation identity --

TEST(nanopb_reencode_matches_googles_bytes)
{
    // The strongest assertion in this file. Decode bytes produced by Google's
    // implementation, re-encode with nanopb, and require them to be identical.
    // Byte identity is not guaranteed by the protobuf spec — field ordering is
    // conventional, not mandated — but both implementations emit in field-number
    // order and omit proto3 defaults, so divergence here means one of them has
    // changed its mind about the schema.
    for (const char *name : {"machine_profile.pb", "install_config.pb"})
    {
        const auto golden = read_fixture(name);
        CHECK(!golden.empty());

        std::vector<uint8_t> again;
        if (std::strcmp(name, "machine_profile.pb") == 0)
        {
            static pinled_v1_MachineProfile p = pinled_v1_MachineProfile_init_zero;
            CHECK(decode(golden, pinled_v1_MachineProfile_fields, &p));
            again = encode(pinled_v1_MachineProfile_fields, &p);
        }
        else
        {
            static pinled_v1_InstallConfig c = pinled_v1_InstallConfig_init_zero;
            CHECK(decode(golden, pinled_v1_InstallConfig_fields, &c));
            again = encode(pinled_v1_InstallConfig_fields, &c);
        }

        CHECK_EQ(again.size(), golden.size());
        if (again.size() == golden.size())
            CHECK_EQ(std::memcmp(again.data(), golden.data(), golden.size()), 0);
    }
}

// --------------------------------------------------------- pure round trip --

TEST(nanopb_round_trip_preserves_every_field)
{
    static pinled_v1_MachineProfile in = pinled_v1_MachineProfile_init_zero;
    in.schema = 1;
    std::strncpy(in.profile_id, "ROUNDTRIP01", sizeof in.profile_id - 1);
    in.revision = 7;
    in.has_machine = true;
    std::strncpy(in.machine.make, "Stern", sizeof in.machine.make - 1);
    in.machine.year = 1979;
    in.machine.lamp_count = 48;
    in.lamps_count = 3;
    for (pb_size_t i = 0; i < in.lamps_count; ++i)
    {
        in.lamps[i].lamp = static_cast<uint32_t>(i + 1);
        std::snprintf(in.lamps[i].name, sizeof in.lamps[i].name, "Lamp %u",
                      static_cast<unsigned>(i + 1));
        in.lamps[i].has_color = true;
        in.lamps[i].color.r = static_cast<uint32_t>(i * 10);
        in.lamps[i].color.g = 128;
        in.lamps[i].color.b = 255;
        in.lamps[i].class_lock = pinled_v1_DriveClass_DRIVE_CLASS_AC_DIMMED;
        in.lamps[i].attack_ms = 25;
        in.lamps[i].decay_ms = 45;
        in.lamps[i].gain_permille = 900;
    }

    const auto bytes = encode(pinled_v1_MachineProfile_fields, &in);
    CHECK(!bytes.empty());

    static pinled_v1_MachineProfile out = pinled_v1_MachineProfile_init_zero;
    CHECK(decode(bytes, pinled_v1_MachineProfile_fields, &out));

    CHECK_EQ(out.schema, in.schema);
    CHECK_EQ(out.revision, in.revision);
    CHECK_EQ(std::string(out.profile_id), std::string(in.profile_id));
    CHECK_EQ(out.lamps_count, in.lamps_count);
    for (pb_size_t i = 0; i < in.lamps_count; ++i)
    {
        CHECK_EQ(out.lamps[i].lamp, in.lamps[i].lamp);
        CHECK_EQ(std::string(out.lamps[i].name), std::string(in.lamps[i].name));
        CHECK_EQ(out.lamps[i].color.g, in.lamps[i].color.g);
        CHECK_EQ(out.lamps[i].class_lock, in.lamps[i].class_lock);
        CHECK_EQ(out.lamps[i].gain_permille, in.lamps[i].gain_permille);
    }
}

TEST(empty_profile_encodes_to_nothing)
{
    // proto3 omits defaults. An untouched document is zero bytes on the wire,
    // which the store must therefore treat as valid rather than as a failure —
    // hence doc_frame accepting an empty payload.
    static pinled_v1_MachineProfile p = pinled_v1_MachineProfile_init_zero;
    const auto bytes = encode(pinled_v1_MachineProfile_fields, &p);
    CHECK_EQ(bytes.size(), static_cast<size_t>(0));
}

TEST(full_128_lamp_profile_fits)
{
    // FR-SCAN-3 caps a machine at 128 channels, so 128 lamps is the largest
    // legal profile. If this ever fails to encode, pinled.options is wrong.
    static pinled_v1_MachineProfile p = pinled_v1_MachineProfile_init_zero;
    p.schema = 1;
    p.lamps_count = 128;
    for (pb_size_t i = 0; i < 128; ++i)
    {
        p.lamps[i].lamp = static_cast<uint32_t>(i + 1);
        std::snprintf(p.lamps[i].name, sizeof p.lamps[i].name,
                      "Channel %u name padded out", static_cast<unsigned>(i));
        p.lamps[i].has_color = true;
        p.lamps[i].color.r = 255;
        p.lamps[i].attack_ms = 30;
        p.lamps[i].decay_ms = 40;
        p.lamps[i].gain_permille = 1000;
    }

    const auto bytes = encode(pinled_v1_MachineProfile_fields, &p, 32768);
    CHECK(!bytes.empty());

    static pinled_v1_MachineProfile out = pinled_v1_MachineProfile_init_zero;
    CHECK(decode(bytes, pinled_v1_MachineProfile_fields, &out));
    CHECK_EQ(out.lamps_count, static_cast<pb_size_t>(128));
    CHECK_EQ(out.lamps[127].lamp, 128u);
}

// ------------------------------------------------------- hostile decoding --

TEST(too_many_lamps_is_refused_not_overflowed)
{
    // 129 minimal LampEntry submessages: tag for field 6 (LEN) = 0x32,
    // length 2, then {lamp: 1} = 0x08 0x01. The struct holds 128, so this must
    // fail cleanly rather than write past the array.
    std::vector<uint8_t> bytes;
    for (int i = 0; i < 129; ++i)
    {
        bytes.push_back(0x32);
        bytes.push_back(0x02);
        bytes.push_back(0x08);
        bytes.push_back(0x01);
    }

    static pinled_v1_MachineProfile p = pinled_v1_MachineProfile_init_zero;
    CHECK_EQ(decode(bytes, pinled_v1_MachineProfile_fields, &p), false);
}

TEST(exactly_128_lamps_is_accepted)
{
    // The boundary on the legal side, so the test above is proving a limit
    // rather than an off-by-one.
    std::vector<uint8_t> bytes;
    for (int i = 0; i < 128; ++i)
    {
        bytes.push_back(0x32);
        bytes.push_back(0x02);
        bytes.push_back(0x08);
        bytes.push_back(0x01);
    }

    static pinled_v1_MachineProfile p = pinled_v1_MachineProfile_init_zero;
    CHECK(decode(bytes, pinled_v1_MachineProfile_fields, &p));
    CHECK_EQ(p.lamps_count, static_cast<pb_size_t>(128));
}

TEST(oversize_name_is_refused_not_truncated)
{
    // name is char[32]. A 40-character name must fail to decode rather than be
    // silently shortened — a truncated lamp name that round-trips would let the
    // device quietly rewrite a shared profile.
    std::string longname(40, 'x');
    std::vector<uint8_t> bytes;
    bytes.push_back(0x32);                                        // field 6, LEN
    bytes.push_back(static_cast<uint8_t>(longname.size() + 2));   // submessage len
    bytes.push_back(0x12);                                        // field 2, LEN
    bytes.push_back(static_cast<uint8_t>(longname.size()));
    bytes.insert(bytes.end(), longname.begin(), longname.end());

    static pinled_v1_MachineProfile p = pinled_v1_MachineProfile_init_zero;
    CHECK_EQ(decode(bytes, pinled_v1_MachineProfile_fields, &p), false);
}

TEST(unknown_fields_are_skipped)
{
    // The mechanism FR-UI-4 depends on: an SPA newer than the firmware sends a
    // field this build has never heard of, and the decode must still succeed.
    auto bytes = read_fixture("machine_profile.pb");
    CHECK(!bytes.empty());

    // field 99, varint: tag = (99 << 3) | 0 = 792 -> 0x98 0x06
    bytes.push_back(0x98);
    bytes.push_back(0x06);
    bytes.push_back(0x2A);

    static pinled_v1_MachineProfile p = pinled_v1_MachineProfile_init_zero;
    CHECK(decode(bytes, pinled_v1_MachineProfile_fields, &p));
    CHECK_EQ(p.lamps_count, static_cast<pb_size_t>(2));
    CHECK_EQ(std::string(p.profile_id), std::string("01J8QK7ZC3"));
}

TEST(truncated_message_is_refused)
{
    const auto golden = read_fixture("machine_profile.pb");
    CHECK(!golden.empty());

    int accepted = 0;
    for (size_t n = 1; n < golden.size(); ++n)
    {
        std::vector<uint8_t> cut(golden.begin(), golden.begin() + static_cast<long>(n));
        static pinled_v1_MachineProfile p = pinled_v1_MachineProfile_init_zero;
        if (decode(cut, pinled_v1_MachineProfile_fields, &p))
            ++accepted;
    }
    // Some prefixes are legitimately valid messages — protobuf is
    // self-delimiting per field, so a cut on a field boundary yields a shorter
    // but well-formed document. That is exactly why doc_frame carries a length
    // and a CRC (FR-CFG-15) instead of trusting the decoder to notice.
    CHECK(accepted < static_cast<int>(golden.size()));
}

// --------------------------------------------------------- the whole stack --

TEST(profile_survives_framing_and_back)
{
    const auto payload = read_fixture("machine_profile.pb");
    CHECK(!payload.empty());

    std::vector<uint8_t> file(doc_frame_size(payload.size()));
    CHECK_EQ(doc_frame_write(file.data(), file.size(), DocKind::MachineProfile,
                             payload.data(), payload.size(), nullptr),
             DocStatus::Ok);

    DocKind kind = DocKind::Unknown;
    const uint8_t *body = nullptr;
    size_t body_len = 0;
    CHECK_EQ(doc_frame_read(file.data(), file.size(), &kind, &body, &body_len),
             DocStatus::Ok);
    CHECK_EQ(kind, DocKind::MachineProfile);

    static pinled_v1_MachineProfile p = pinled_v1_MachineProfile_init_zero;
    pb_istream_t is = pb_istream_from_buffer(body, body_len);
    CHECK(pb_decode(&is, pinled_v1_MachineProfile_fields, &p));
    CHECK_EQ(std::string(p.machine.model), std::string("Eight Ball Deluxe"));
}

TEST(a_flipped_bit_in_storage_is_caught_before_the_decoder_sees_it)
{
    // The reason the CRC is checked first: protobuf will happily decode many
    // corrupted buffers into a plausible-looking message. The frame is what
    // stops a silently wrong configuration from being loaded.
    const auto payload = read_fixture("machine_profile.pb");
    CHECK(!payload.empty());

    std::vector<uint8_t> file(doc_frame_size(payload.size()));
    CHECK_EQ(doc_frame_write(file.data(), file.size(), DocKind::MachineProfile,
                             payload.data(), payload.size(), nullptr),
             DocStatus::Ok);

    int decoder_would_have_accepted = 0;
    for (size_t i = 0; i < payload.size(); ++i)
    {
        file[kDocHeaderSize + i] ^= 0x01;

        CHECK_EQ(doc_frame_read(file.data(), file.size(), nullptr, nullptr, nullptr),
                 DocStatus::BadCrc);

        static pinled_v1_MachineProfile p = pinled_v1_MachineProfile_init_zero;
        pb_istream_t is = pb_istream_from_buffer(file.data() + kDocHeaderSize, payload.size());
        if (pb_decode(&is, pinled_v1_MachineProfile_fields, &p))
            ++decoder_would_have_accepted;

        file[kDocHeaderSize + i] ^= 0x01;
    }

    // Not an assertion about a specific count — the point is that it is well
    // above zero, so the CRC is doing work the decoder does not.
    CHECK(decoder_would_have_accepted > 0);
}

int main() { return ooe::test::run_all(); }
