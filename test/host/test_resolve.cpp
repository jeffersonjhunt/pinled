/**
 * @file test_resolve.cpp
 * @brief The projection: two documents in, per-channel runtime records out.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * The join is where a shared profile either lands correctly on someone else's
 * machine or corrupts it, so the interesting cases are the asymmetric ones: a
 * profile describing lamps this install has not wired, an install wiring lamps
 * this profile has never heard of, and the half-finished states that are
 * normal during commissioning rather than errors.
 */

#include "harness.h"

#include "pinled_channel_config.h"
#include "pinled_doc_frame.h"

#include <pb_decode.h>

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace ooe::pinled;

namespace
{
    constexpr size_t kCap = 128;

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

    /// A minimal 1-module install: 16 channels, 16 LEDs, no wiring.
    pinled_v1_InstallConfig bare_install(uint32_t modules = 1, uint32_t per = 16,
                                         uint32_t leds = 16)
    {
        pinled_v1_InstallConfig c = pinled_v1_InstallConfig_init_zero;
        c.schema = 1;
        c.has_geometry = true;
        c.geometry.num_modules = modules;
        c.geometry.channels_per_module = per;
        c.geometry.led_count = leds;
        return c;
    }

    void wire(pinled_v1_InstallConfig &c, uint32_t channel, uint32_t lamp, int32_t led)
    {
        c.wiring[c.wiring_count].channel = channel;
        c.wiring[c.wiring_count].lamp = lamp;
        c.wiring[c.wiring_count].led_index = led;
        ++c.wiring_count;
    }

    void style(pinled_v1_MachineProfile &p, uint32_t lamp, uint32_t r, uint32_t g,
               uint32_t b, pinled_v1_DriveClass cls = pinled_v1_DriveClass_DRIVE_CLASS_UNSPECIFIED,
               uint32_t attack = 0, uint32_t decay = 0, uint32_t gain = 0)
    {
        pinled_v1_LampEntry &e = p.lamps[p.lamps_count];
        e.lamp = lamp;
        e.has_color = true;
        e.color.r = r;
        e.color.g = g;
        e.color.b = b;
        e.class_lock = cls;
        e.attack_ms = attack;
        e.decay_ms = decay;
        e.gain_permille = gain;
        ++p.lamps_count;
    }
} // namespace

// ------------------------------------------------------------- the basics --

TEST(resolve_maps_wired_and_styled_channels)
{
    auto install = bare_install();
    wire(install, 3, 17, 3);

    pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;
    style(profile, 17, 0, 80, 255, pinled_v1_DriveClass_DRIVE_CLASS_STEADY, 25, 45, 900);

    static ChannelConfig out[kCap];
    size_t n = 0;
    CHECK_EQ(resolve(profile, install, out, kCap, &n), ResolveStatus::Ok);
    CHECK_EQ(n, static_cast<size_t>(16));

    CHECK_EQ(out[3].lamp, static_cast<uint16_t>(17));
    CHECK_EQ(out[3].led_index, static_cast<int16_t>(3));
    CHECK_EQ(out[3].r, static_cast<uint8_t>(0));
    CHECK_EQ(out[3].g, static_cast<uint8_t>(80));
    CHECK_EQ(out[3].b, static_cast<uint8_t>(255));
    CHECK_EQ(out[3].class_lock, DriveClass::STEADY);
    CHECK_EQ(out[3].attack_ms, static_cast<uint16_t>(25));
    CHECK_EQ(out[3].decay_ms, static_cast<uint16_t>(45));
    CHECK_EQ(out[3].gain_permille, static_cast<uint16_t>(900));
    CHECK(out[3].bound());
    CHECK(out[3].mapped());
}

TEST(every_slot_is_written_even_unbound_ones)
{
    // A caller reusing a buffer across a reconfigure must never read a stale
    // entry, so resolve() writes the whole range rather than only what changed.
    static ChannelConfig out[kCap];
    for (size_t i = 0; i < kCap; ++i)
    {
        out[i].lamp = 999;
        out[i].led_index = 42;
        out[i].r = 1;
    }

    auto install = bare_install();
    pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;
    size_t n = 0;
    CHECK_EQ(resolve(profile, install, out, kCap, &n), ResolveStatus::Ok);

    for (size_t i = 0; i < n; ++i)
    {
        CHECK_EQ(out[i].lamp, static_cast<uint16_t>(0));
        CHECK_EQ(out[i].led_index, static_cast<int16_t>(-1));
        CHECK(!out[i].bound());
        CHECK(!out[i].mapped());
    }
    // Beyond the geometry the buffer is untouched, by design — the caller was
    // told how many channels are live.
    CHECK_EQ(out[n].lamp, static_cast<uint16_t>(999));
}

TEST(unbound_channels_still_carry_resolved_defaults)
{
    // Binding a channel later must not change how it behaves beyond gaining a
    // lamp, so an unbound slot is fully settled rather than zeroed.
    auto install = bare_install();
    pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;

    static ChannelConfig out[kCap];
    size_t n = 0;
    CHECK_EQ(resolve(profile, install, out, kCap, &n), ResolveStatus::Ok);

    const ResolveDefaults d;
    CHECK_EQ(out[0].attack_ms, d.attack_ms);
    CHECK_EQ(out[0].decay_ms, d.decay_ms);
    CHECK_EQ(out[0].gain_permille, d.gain_permille);
    CHECK_EQ(out[0].r, d.r);
    CHECK_NE(out[0].attack_ms, static_cast<uint16_t>(0));
}

// ---------------------------------------------- the asymmetries, permitted --

TEST(a_wired_lamp_with_no_profile_entry_keeps_defaults)
{
    // You wire before you style. This is the state of every channel the moment
    // it is bound in the UI, so it cannot be an error.
    auto install = bare_install();
    wire(install, 2, 40, 2);
    pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;

    static ChannelConfig out[kCap];
    size_t n = 0;
    CHECK_EQ(resolve(profile, install, out, kCap, &n), ResolveStatus::Ok);

    const ResolveDefaults d;
    CHECK_EQ(out[2].lamp, static_cast<uint16_t>(40));
    CHECK_EQ(out[2].r, d.r);
    CHECK_EQ(out[2].class_lock, DriveClass::UNKNOWN);
}

TEST(profile_entries_for_unwired_lamps_are_ignored)
{
    // The common case for a shared profile: it describes all 60 lamps of the
    // machine and this install has wired 2 of them so far.
    auto install = bare_install();
    wire(install, 0, 17, 0);

    pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;
    for (uint32_t lamp = 1; lamp <= 60; ++lamp)
        style(profile, lamp, lamp, 0, 0);

    static ChannelConfig out[kCap];
    size_t n = 0;
    CHECK_EQ(resolve(profile, install, out, kCap, &n), ResolveStatus::Ok);

    CHECK_EQ(out[0].lamp, static_cast<uint16_t>(17));
    CHECK_EQ(out[0].r, static_cast<uint8_t>(17));
    CHECK_EQ(out[1].lamp, static_cast<uint16_t>(0));
}

TEST(a_channel_absent_from_wiring_is_unbound_not_an_error)
{
    auto install = bare_install();
    wire(install, 5, 1, 5);
    pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;

    static ChannelConfig out[kCap];
    size_t n = 0;
    CHECK_EQ(resolve(profile, install, out, kCap, &n), ResolveStatus::Ok);
    CHECK(out[5].bound());
    CHECK(!out[4].bound());
}

TEST(a_wiring_entry_may_map_an_led_before_a_lamp_is_known)
{
    // lamp 0 is the proto default, so {channel, led_index} with no lamp is a
    // legitimate half-step: the string position is known, the lamp is not.
    auto install = bare_install();
    wire(install, 7, 0, 7);
    pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;

    static ChannelConfig out[kCap];
    size_t n = 0;
    CHECK_EQ(resolve(profile, install, out, kCap, &n), ResolveStatus::Ok);
    CHECK_EQ(out[7].led_index, static_cast<int16_t>(7));
    CHECK(!out[7].bound());
    CHECK(out[7].mapped());
}

// --------------------------------------------------------------- inherit ---

TEST(zero_inherits_from_the_install_defaults)
{
    auto install = bare_install();
    install.has_filament = true;
    install.filament.attack_ms = 12;
    install.filament.decay_ms = 34;
    install.filament.gain_permille = 800;
    wire(install, 0, 5, 0);

    pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;
    style(profile, 5, 1, 2, 3); // all tuning left at 0 = inherit

    static ChannelConfig out[kCap];
    size_t n = 0;
    CHECK_EQ(resolve(profile, install, out, kCap, &n), ResolveStatus::Ok);
    CHECK_EQ(out[0].attack_ms, static_cast<uint16_t>(12));
    CHECK_EQ(out[0].decay_ms, static_cast<uint16_t>(34));
    CHECK_EQ(out[0].gain_permille, static_cast<uint16_t>(800));
}

TEST(a_per_lamp_value_beats_the_install_default)
{
    auto install = bare_install();
    install.has_filament = true;
    install.filament.attack_ms = 12;
    wire(install, 0, 5, 0);

    pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;
    style(profile, 5, 1, 2, 3, pinled_v1_DriveClass_DRIVE_CLASS_UNSPECIFIED, 99);

    static ChannelConfig out[kCap];
    size_t n = 0;
    CHECK_EQ(resolve(profile, install, out, kCap, &n), ResolveStatus::Ok);
    CHECK_EQ(out[0].attack_ms, static_cast<uint16_t>(99));
}

TEST(an_install_zero_inherits_in_turn_from_the_fallback)
{
    // "Unset" has to be expressible at every level without a sentinel value,
    // so a zero in the install config inherits too.
    auto install = bare_install();
    install.has_filament = true;
    install.filament.attack_ms = 0;
    install.filament.decay_ms = 34;
    wire(install, 0, 5, 0);

    pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;

    static ChannelConfig out[kCap];
    size_t n = 0;
    ResolveDefaults fb;
    fb.attack_ms = 7;
    CHECK_EQ(resolve(profile, install, out, kCap, &n, fb), ResolveStatus::Ok);
    CHECK_EQ(out[0].attack_ms, static_cast<uint16_t>(7));
    CHECK_EQ(out[0].decay_ms, static_cast<uint16_t>(34));
}

TEST(no_resolved_value_is_ever_left_as_inherit)
{
    // The contract downstream relies on: after resolve(), zero never means
    // anything special, because it cannot occur.
    const auto pbytes = read_fixture("machine_profile.pb");
    const auto ibytes = read_fixture("install_config.pb");
    CHECK(!pbytes.empty());
    CHECK(!ibytes.empty());

    static pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;
    static pinled_v1_InstallConfig install = pinled_v1_InstallConfig_init_zero;
    pb_istream_t p1 = pb_istream_from_buffer(pbytes.data(), pbytes.size());
    pb_istream_t p2 = pb_istream_from_buffer(ibytes.data(), ibytes.size());
    CHECK(pb_decode(&p1, pinled_v1_MachineProfile_fields, &profile));
    CHECK(pb_decode(&p2, pinled_v1_InstallConfig_fields, &install));

    static ChannelConfig out[kCap];
    size_t n = 0;
    CHECK_EQ(resolve(profile, install, out, kCap, &n), ResolveStatus::Ok);
    CHECK_EQ(n, static_cast<size_t>(64));

    int zeros = 0;
    for (size_t i = 0; i < n; ++i)
        if (out[i].attack_ms == 0 || out[i].decay_ms == 0 || out[i].gain_permille == 0)
            ++zeros;
    CHECK_EQ(zeros, 0);
}

// ------------------------------------------------------ the golden fixtures --

TEST(the_committed_documents_resolve_as_expected)
{
    const auto pbytes = read_fixture("machine_profile.pb");
    const auto ibytes = read_fixture("install_config.pb");
    CHECK(!pbytes.empty());
    CHECK(!ibytes.empty());

    static pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;
    static pinled_v1_InstallConfig install = pinled_v1_InstallConfig_init_zero;
    pb_istream_t p1 = pb_istream_from_buffer(pbytes.data(), pbytes.size());
    pb_istream_t p2 = pb_istream_from_buffer(ibytes.data(), ibytes.size());
    CHECK(pb_decode(&p1, pinled_v1_MachineProfile_fields, &profile));
    CHECK(pb_decode(&p2, pinled_v1_InstallConfig_fields, &install));

    static ChannelConfig out[kCap];
    size_t n = 0;
    CHECK_EQ(resolve(profile, install, out, kCap, &n), ResolveStatus::Ok);

    // wiring: ch 0 -> lamp 17 -> "Left Drop Target", blue, STEADY, 30/40/1000
    CHECK_EQ(out[0].lamp, static_cast<uint16_t>(17));
    CHECK_EQ(out[0].led_index, static_cast<int16_t>(0));
    CHECK_EQ(out[0].g, static_cast<uint8_t>(80));
    CHECK_EQ(out[0].class_lock, DriveClass::STEADY);
    CHECK_EQ(out[0].attack_ms, static_cast<uint16_t>(30));

    // wiring: ch 4 -> lamp 45 -> GI, warm, AC_STEADY, all tuning inherited
    CHECK_EQ(out[4].lamp, static_cast<uint16_t>(45));
    CHECK_EQ(out[4].class_lock, DriveClass::AC_STEADY);
    CHECK_EQ(out[4].r, static_cast<uint8_t>(255));
    const ResolveDefaults d;
    CHECK_EQ(out[4].attack_ms, d.attack_ms);
    CHECK_EQ(out[4].gain_permille, d.gain_permille);

    // everything else in the 64-channel geometry is unbound
    CHECK(!out[1].bound());
    CHECK(!out[63].bound());
}

// ------------------------------------------------------- the rejections ----

TEST(a_duplicate_channel_is_rejected)
{
    auto install = bare_install();
    wire(install, 3, 17, 3);
    wire(install, 3, 18, 4);
    pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;

    static ChannelConfig out[kCap];
    size_t n = 0;
    CHECK_EQ(resolve(profile, install, out, kCap, &n), ResolveStatus::DuplicateChannel);
}

TEST(duplicate_placeholder_entries_are_also_rejected)
{
    // The duplicate the obvious implementation misses. Inferring "this channel
    // is claimed" from lamp != 0 or led_index != -1 works for every ordinary
    // entry and silently accepts two placeholders — {channel, lamp 0, led -1}
    // twice — because neither leaves a mark. Hence an explicit claimed-set.
    auto install = bare_install();
    wire(install, 6, 0, -1);
    wire(install, 6, 0, -1);
    pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;

    static ChannelConfig out[kCap];
    size_t n = 0;
    CHECK_EQ(resolve(profile, install, out, kCap, &n), ResolveStatus::DuplicateChannel);
}

TEST(a_rejected_document_does_not_half_update_the_output)
{
    // Two lamps styled, the second unrepresentable. The first must not have
    // been applied — a caller that ignores the status should not be able to
    // half-load a configuration.
    auto install = bare_install();
    wire(install, 0, 1, 0);
    wire(install, 1, 2, 1);
    pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;
    style(profile, 1, 10, 20, 30);
    style(profile, 2, 999, 0, 0);

    static ChannelConfig out[kCap];
    size_t n = 0;
    CHECK_EQ(resolve(profile, install, out, kCap, &n), ResolveStatus::ValueOutOfRange);
    // channel 1's colour was never touched, even partially
    const ResolveDefaults d;
    CHECK_EQ(out[1].r, d.r);
}

TEST(a_channel_beyond_the_geometry_is_rejected)
{
    auto install = bare_install(1, 16, 16);
    wire(install, 16, 1, 0);
    pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;

    static ChannelConfig out[kCap];
    size_t n = 0;
    CHECK_EQ(resolve(profile, install, out, kCap, &n), ResolveStatus::ChannelOutOfRange);
}

TEST(an_led_index_beyond_the_string_is_rejected)
{
    auto install = bare_install(1, 16, 8);
    wire(install, 0, 1, 8);
    pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;

    static ChannelConfig out[kCap];
    size_t n = 0;
    CHECK_EQ(resolve(profile, install, out, kCap, &n), ResolveStatus::LedOutOfRange);
}

TEST(minus_one_is_the_only_legal_negative_led_index)
{
    auto install = bare_install();
    wire(install, 0, 1, -1);
    pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;

    static ChannelConfig out[kCap];
    size_t n = 0;
    CHECK_EQ(resolve(profile, install, out, kCap, &n), ResolveStatus::Ok);
    CHECK(!out[0].mapped());
    CHECK(out[0].bound());

    install.wiring[0].led_index = -2;
    CHECK_EQ(resolve(profile, install, out, kCap, &n), ResolveStatus::LedOutOfRange);
}

TEST(a_geometry_larger_than_the_hardware_ceiling_is_rejected)
{
    // FR-SCAN-3 caps a chain at 8 modules x 16 channels.
    auto install = bare_install(9, 16, 144);
    pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;

    static ChannelConfig out[200];
    size_t n = 0;
    CHECK_EQ(resolve(profile, install, out, 200, &n), ResolveStatus::GeometryTooLarge);
}

TEST(a_geometry_larger_than_the_buffer_is_rejected)
{
    auto install = bare_install(4, 16, 64);
    pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;

    static ChannelConfig out[kCap];
    size_t n = 0;
    CHECK_EQ(resolve(profile, install, out, 32, &n), ResolveStatus::GeometryTooLarge);
    CHECK_EQ(n, static_cast<size_t>(0));
}

TEST(a_colour_above_255_is_rejected_rather_than_clamped)
{
    // Clamping would invent data. A document that cannot be represented is a
    // document that should be refused.
    auto install = bare_install();
    wire(install, 0, 5, 0);
    pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;
    style(profile, 5, 256, 0, 0);

    static ChannelConfig out[kCap];
    size_t n = 0;
    CHECK_EQ(resolve(profile, install, out, kCap, &n), ResolveStatus::ValueOutOfRange);
}

TEST(an_unrepresentable_tau_is_rejected)
{
    auto install = bare_install();
    wire(install, 0, 5, 0);
    pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;
    style(profile, 5, 1, 2, 3, pinled_v1_DriveClass_DRIVE_CLASS_UNSPECIFIED, 70000);

    static ChannelConfig out[kCap];
    size_t n = 0;
    CHECK_EQ(resolve(profile, install, out, kCap, &n), ResolveStatus::ValueOutOfRange);
}

TEST(an_unrepresentable_install_default_is_rejected)
{
    auto install = bare_install();
    install.has_filament = true;
    install.filament.decay_ms = 100000;
    pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;

    static ChannelConfig out[kCap];
    size_t n = 0;
    CHECK_EQ(resolve(profile, install, out, kCap, &n), ResolveStatus::ValueOutOfRange);
}

// ----------------------------------------------------------- degenerate ----

TEST(an_empty_install_resolves_to_zero_channels)
{
    // A device with nothing stored yet. Not an error — it boots to Kconfig
    // defaults (FR-CFG-4) and waits to be configured.
    pinled_v1_InstallConfig install = pinled_v1_InstallConfig_init_zero;
    pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;

    static ChannelConfig out[kCap];
    size_t n = 99;
    CHECK_EQ(resolve(profile, install, out, kCap, &n), ResolveStatus::Ok);
    CHECK_EQ(n, static_cast<size_t>(0));
}

TEST(a_null_output_is_only_an_error_when_channels_exist)
{
    pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;

    pinled_v1_InstallConfig empty = pinled_v1_InstallConfig_init_zero;
    size_t n = 0;
    CHECK_EQ(resolve(profile, empty, nullptr, 0, &n), ResolveStatus::Ok);

    auto sized = bare_install();
    CHECK_EQ(resolve(profile, sized, nullptr, 0, &n), ResolveStatus::GeometryTooLarge);
}

TEST(the_full_128_channel_geometry_resolves)
{
    auto install = bare_install(8, 16, 128);
    pinled_v1_MachineProfile profile = pinled_v1_MachineProfile_init_zero;
    for (uint32_t ch = 0; ch < 128; ++ch)
        wire(install, ch, ch + 1, static_cast<int32_t>(ch));
    for (uint32_t lamp = 1; lamp <= 128; ++lamp)
        style(profile, lamp, 10, 20, 30, pinled_v1_DriveClass_DRIVE_CLASS_MATRIX, 15, 25, 1200);

    static ChannelConfig out[kCap];
    size_t n = 0;
    CHECK_EQ(resolve(profile, install, out, kCap, &n), ResolveStatus::Ok);
    CHECK_EQ(n, static_cast<size_t>(128));
    CHECK_EQ(out[127].lamp, static_cast<uint16_t>(128));
    CHECK_EQ(out[127].class_lock, DriveClass::MATRIX);
    CHECK_EQ(out[127].gain_permille, static_cast<uint16_t>(1200));
}

// ------------------------------------------------------------ enum bridge --

TEST(drive_class_round_trips_through_the_wire_enum)
{
    const DriveClass all[] = {DriveClass::UNKNOWN, DriveClass::OFF, DriveClass::STEADY,
                              DriveClass::MATRIX, DriveClass::AC_STEADY, DriveClass::AC_DIMMED};
    for (DriveClass c : all)
        CHECK_EQ(drive_class_from_proto(drive_class_to_proto(c)), c);
}

TEST(an_unrecognised_wire_class_becomes_unknown_not_a_guess)
{
    // A newer SPA can send a class this build has never heard of. Leaving it to
    // the profiler is the only safe reading — FR-UI-4 in miniature.
    //
    // 6 and 7, not some large number: the enum's largest value is 5, so its
    // representable range is 0..7 and anything above that is an unspecified
    // conversion rather than a test. These are the next two values a future
    // schema would actually use.
    CHECK_EQ(drive_class_from_proto(static_cast<pinled_v1_DriveClass>(6)),
             DriveClass::UNKNOWN);
    CHECK_EQ(drive_class_from_proto(static_cast<pinled_v1_DriveClass>(7)),
             DriveClass::UNKNOWN);
}

TEST(drive_class_str_covers_every_value)
{
    const DriveClass all[] = {DriveClass::UNKNOWN, DriveClass::OFF, DriveClass::STEADY,
                              DriveClass::MATRIX, DriveClass::AC_STEADY, DriveClass::AC_DIMMED};
    for (DriveClass c : all)
        CHECK(std::strcmp(drive_class_str(c), "?") != 0);
}

TEST(resolve_status_str_covers_every_value)
{
    const ResolveStatus all[] = {
        ResolveStatus::Ok, ResolveStatus::NullOutput, ResolveStatus::GeometryTooLarge,
        ResolveStatus::ChannelOutOfRange, ResolveStatus::DuplicateChannel,
        ResolveStatus::LedOutOfRange, ResolveStatus::ValueOutOfRange};
    for (ResolveStatus s : all)
        CHECK(std::strcmp(resolve_status_str(s), "unknown") != 0);
}

int main() { return ooe::test::run_all(); }
