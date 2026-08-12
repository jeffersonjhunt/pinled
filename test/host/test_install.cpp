/**
 * @file test_install.cpp
 * @brief The install document projected onto the device-wide record.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * `test_resolve` covers the per-channel half of the projection; this covers the
 * device-wide half. Most of it is one question asked field by field: what does
 * a zero mean here? proto3 gives scalars no presence, so "unset" and "zero" are
 * the same bytes, and getting the answer wrong for a single field either
 * silently discards a setting or silently invents one.
 *
 * The case with a hardware consequence is `Pins`: honouring an unfilled Pins
 * message literally would drive GPIO 0 — a strapping pin — at boot.
 */

#include "harness.h"

#include "pinled_resolve.h"

#include <cmath>

using namespace ooe::pinled;

namespace
{
    bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }

    /// Distinct from every default, so an assertion cannot pass by coincidence.
    MachineConfig fallback()
    {
        MachineConfig c{};
        c.clk_pin = 11;
        c.data_pin = 12;
        c.pl_pin = 13;
        c.led_pin = 14;
        c.spi_hz = 2000000;
        c.spi_mode = 2;
        c.pl_from_cs = true;
        c.num_modules = 2;
        c.channels_per_module = 16;
        c.active_low = false;
        c.sample_rate_hz = 2000.0f;
        c.refresh_hz = 90;
        c.attack_ms = 30.0f;
        c.decay_ms = 40.0f;
        c.gamma = 2.2f;
        c.led_count = 16;
        return c;
    }

    pinled_v1_InstallConfig blank()
    {
        pinled_v1_InstallConfig i = pinled_v1_InstallConfig_init_zero;
        return i;
    }
} // namespace

// ------------------------------------------------------------- absent bits --

TEST(an_empty_document_changes_nothing)
{
    // A device with an install config that says nothing must behave exactly
    // like one with no install config at all (FR-CFG-4).
    const pinled_v1_InstallConfig in = blank();
    MachineConfig out{};
    CHECK(install_to_machine(in, out, fallback()) == InstallStatus::Ok);

    const MachineConfig f = fallback();
    CHECK_EQ(out.num_modules, f.num_modules);
    CHECK_EQ(out.led_count, f.led_count);
    CHECK_EQ(out.clk_pin, f.clk_pin);
    CHECK_EQ(out.spi_hz, f.spi_hz);
    CHECK_EQ(out.refresh_hz, f.refresh_hz);
    CHECK(near(out.gamma, f.gamma));
    CHECK(near(out.sample_rate_hz, f.sample_rate_hz));
}

TEST(a_present_but_zeroed_geometry_inherits_every_field)
{
    // has_geometry with all-zero counts. Zero modules is not a machine, so
    // every one of these has to inherit rather than be taken literally.
    pinled_v1_InstallConfig in = blank();
    in.has_geometry = true;
    MachineConfig out{};
    CHECK(install_to_machine(in, out, fallback()) == InstallStatus::Ok);
    CHECK_EQ(out.num_modules, fallback().num_modules);
    CHECK_EQ(out.channels_per_module, fallback().channels_per_module);
    CHECK_EQ(out.led_count, fallback().led_count);
}

// ---------------------------------------------------------------- geometry --

TEST(geometry_is_taken_field_by_field)
{
    pinled_v1_InstallConfig in = blank();
    in.has_geometry = true;
    in.geometry.num_modules = 4;
    in.geometry.led_count = 60;
    // channels_per_module left zero: inherits.

    MachineConfig out{};
    CHECK(install_to_machine(in, out, fallback()) == InstallStatus::Ok);
    CHECK_EQ(out.num_modules, static_cast<size_t>(4));
    CHECK_EQ(out.channels_per_module, static_cast<size_t>(16));
    CHECK_EQ(out.led_count, static_cast<size_t>(60));
    CHECK_EQ(out.total_channels(), static_cast<size_t>(64));
}

TEST(geometry_past_the_channel_ceiling_is_rejected)
{
    pinled_v1_InstallConfig in = blank();
    in.has_geometry = true;
    in.geometry.num_modules = 9; // 9 x 16 = 144 > 128 (FR-SCAN-3)
    MachineConfig out{};
    CHECK(install_to_machine(in, out, fallback()) == InstallStatus::GeometryTooLarge);
}

TEST(exactly_the_channel_ceiling_is_accepted)
{
    pinled_v1_InstallConfig in = blank();
    in.has_geometry = true;
    in.geometry.num_modules = 8;
    in.geometry.channels_per_module = 16;
    MachineConfig out{};
    CHECK(install_to_machine(in, out, fallback()) == InstallStatus::Ok);
    CHECK_EQ(out.total_channels(), static_cast<size_t>(128));
}

TEST(an_absurd_led_count_is_rejected_before_it_becomes_an_allocation)
{
    // led_count sizes a buffer in LampMap::init(). Unbounded, a corrupt
    // document is an out-of-memory at boot rather than a rejected document.
    pinled_v1_InstallConfig in = blank();
    in.has_geometry = true;
    in.geometry.led_count = kMaxLedCount + 1;
    MachineConfig out{};
    CHECK(install_to_machine(in, out, fallback()) == InstallStatus::GeometryTooLarge);
}

// -------------------------------------------------------------------- pins --

TEST(pins_are_taken_literally_when_specified)
{
    pinled_v1_InstallConfig in = blank();
    in.has_pins = true;
    in.pins.clk = 5;
    in.pins.pl = 6;
    in.pins.data = 7;
    in.pins.led = 8;

    MachineConfig out{};
    CHECK(install_to_machine(in, out, fallback()) == InstallStatus::Ok);
    CHECK_EQ(out.clk_pin, 5);
    CHECK_EQ(out.pl_pin, 6);
    CHECK_EQ(out.data_pin, 7);
    CHECK_EQ(out.led_pin, 8);
}

TEST(gpio_zero_is_a_real_pin_when_the_others_are_set)
{
    // The reason the all-zero rule is about the message and not about each
    // field: GPIO 0 must remain assignable.
    pinled_v1_InstallConfig in = blank();
    in.has_pins = true;
    in.pins.clk = 0;
    in.pins.pl = 6;
    in.pins.data = 7;
    in.pins.led = 8;

    MachineConfig out{};
    CHECK(install_to_machine(in, out, fallback()) == InstallStatus::Ok);
    CHECK_EQ(out.clk_pin, 0);
}

TEST(an_all_zero_pins_message_inherits_instead_of_driving_gpio_zero)
{
    // THE case with a hardware consequence. Four signals cannot share one pin,
    // so this document cannot be legitimate — but a writer that emitted Pins
    // without filling it in is entirely plausible, and honouring it would put
    // CLK, DATA, /PL and the LED string all on a strapping pin at boot.
    pinled_v1_InstallConfig in = blank();
    in.has_pins = true;

    MachineConfig out{};
    CHECK(install_to_machine(in, out, fallback()) == InstallStatus::Ok);
    CHECK_EQ(out.clk_pin, fallback().clk_pin);
    CHECK_EQ(out.pl_pin, fallback().pl_pin);
    CHECK_EQ(out.data_pin, fallback().data_pin);
    CHECK_EQ(out.led_pin, fallback().led_pin);
}

TEST(minus_one_stays_not_fitted)
{
    pinled_v1_InstallConfig in = blank();
    in.has_pins = true;
    in.pins.clk = 5;
    in.pins.pl = -1; // bench rig with no /PL
    in.pins.data = 7;
    in.pins.led = 8;

    MachineConfig out{};
    CHECK(install_to_machine(in, out, fallback()) == InstallStatus::Ok);
    CHECK_EQ(out.pl_pin, kPinNotFitted);
}

TEST(a_pin_number_no_gpio_could_have_is_rejected)
{
    pinled_v1_InstallConfig in = blank();
    in.has_pins = true;
    in.pins.clk = 5;
    in.pins.pl = 6;
    in.pins.data = 7;
    in.pins.led = 999;

    MachineConfig out{};
    CHECK(install_to_machine(in, out, fallback()) == InstallStatus::PinOutOfRange);

    in.pins.led = -2; // below "not fitted"
    CHECK(install_to_machine(in, out, fallback()) == InstallStatus::PinOutOfRange);
}

// -------------------------------------------------------------------- scan --

TEST(scan_rates_inherit_on_zero_and_apply_otherwise)
{
    pinled_v1_InstallConfig in = blank();
    in.has_scan = true;
    in.scan.sample_rate_hz = 5000;
    // spi_hz left zero: inherits.

    MachineConfig out{};
    CHECK(install_to_machine(in, out, fallback()) == InstallStatus::Ok);
    CHECK(near(out.sample_rate_hz, 5000.0f));
    CHECK_EQ(out.spi_hz, fallback().spi_hz);
}

TEST(spi_mode_zero_is_a_real_mode_and_is_taken_literally)
{
    // Mode 0 is a legal SPI mode, so it cannot mean "inherit" — which makes
    // the range check the only thing guarding this field.
    pinled_v1_InstallConfig in = blank();
    in.has_scan = true;
    in.scan.spi_mode = 0;

    MachineConfig out{};
    CHECK(install_to_machine(in, out, fallback()) == InstallStatus::Ok);
    CHECK_EQ(out.spi_mode, static_cast<uint8_t>(0));
}

TEST(an_impossible_spi_mode_is_rejected)
{
    pinled_v1_InstallConfig in = blank();
    in.has_scan = true;
    in.scan.spi_mode = 4;
    MachineConfig out{};
    CHECK(install_to_machine(in, out, fallback()) == InstallStatus::ValueOutOfRange);
}

TEST(scan_flags_are_stated_by_a_present_scan_message)
{
    // Booleans have no presence either. A present ScanConfig states both flags;
    // that is what "present" has to mean for a flag.
    pinled_v1_InstallConfig in = blank();
    in.has_scan = true;
    in.scan.active_low = true;
    in.scan.pl_from_cs = false;

    MachineConfig out{};
    CHECK(install_to_machine(in, out, fallback()) == InstallStatus::Ok);
    CHECK(out.active_low);
    CHECK(!out.pl_from_cs);
}

TEST(out_of_range_rates_are_rejected)
{
    MachineConfig out{};

    pinled_v1_InstallConfig fast = blank();
    fast.has_scan = true;
    fast.scan.sample_rate_hz = 1000000;
    CHECK(install_to_machine(fast, out, fallback()) == InstallStatus::ValueOutOfRange);

    pinled_v1_InstallConfig slow = blank();
    slow.has_scan = true;
    slow.scan.spi_hz = 100; // below any usable chain clock
    CHECK(install_to_machine(slow, out, fallback()) == InstallStatus::ValueOutOfRange);
}

// ------------------------------------------------------------------ render --

TEST(gamma_is_scaled_by_a_hundred)
{
    pinled_v1_InstallConfig in = blank();
    in.has_render = true;
    in.render.gamma_x100 = 180;
    in.render.refresh_hz = 120;

    MachineConfig out{};
    CHECK(install_to_machine(in, out, fallback()) == InstallStatus::Ok);
    CHECK(near(out.gamma, 1.8f));
    CHECK_EQ(out.refresh_hz, static_cast<uint32_t>(120));
}

TEST(the_strip_byte_order_reaches_the_device_record)
{
    pinled_v1_InstallConfig in = blank();
    in.has_render = true;
    in.render.color_order = pinled_v1_ColorOrder_COLOR_ORDER_RGB;

    MachineConfig out{};
    CHECK(install_to_machine(in, out, fallback()) == InstallStatus::Ok);
    CHECK(out.color_order == ColorOrder::RGB);
}

TEST(an_absent_color_order_stays_grb)
{
    // The compatibility rule made concrete: every document written before the
    // field existed must keep behaving exactly as it did.
    pinled_v1_InstallConfig in = blank();
    in.has_render = true;
    in.render.refresh_hz = 120;

    MachineConfig out{};
    CHECK(install_to_machine(in, out, fallback()) == InstallStatus::Ok);
    CHECK(out.color_order == ColorOrder::UNSPECIFIED);
    CHECK_EQ(pack_for_order(out.color_order, 1, 2, 3),
             pack_for_order(ColorOrder::GRB, 1, 2, 3));
}

TEST(a_color_order_from_a_newer_schema_falls_back_to_grb)
{
    // Unknown enum values survive on the wire but cannot be honoured. Guessing
    // would mis-colour a whole playfield; GRB is the documented default.
    pinled_v1_InstallConfig in = blank();
    in.has_render = true;
    in.render.color_order = static_cast<pinled_v1_ColorOrder>(7);

    MachineConfig out{};
    CHECK(install_to_machine(in, out, fallback()) == InstallStatus::Ok);
    CHECK(out.color_order == ColorOrder::UNSPECIFIED);
}

TEST(an_unrepresentable_gamma_is_rejected)
{
    MachineConfig out{};
    pinled_v1_InstallConfig in = blank();
    in.has_render = true;
    in.render.gamma_x100 = 5000;
    CHECK(install_to_machine(in, out, fallback()) == InstallStatus::ValueOutOfRange);
}

// -------------------------------------------------------- filament defaults --

TEST(filament_defaults_reach_the_device_record)
{
    pinled_v1_InstallConfig in = blank();
    in.has_filament = true;
    in.filament.attack_ms = 55;
    // decay left zero: inherits.

    MachineConfig out{};
    CHECK(install_to_machine(in, out, fallback()) == InstallStatus::Ok);
    CHECK(near(out.attack_ms, 55.0f));
    CHECK(near(out.decay_ms, 40.0f));
}

TEST(a_tau_that_cannot_be_represented_is_rejected)
{
    MachineConfig out{};
    pinled_v1_InstallConfig in = blank();
    in.has_filament = true;
    in.filament.decay_ms = 70000; // > uint16
    CHECK(install_to_machine(in, out, fallback()) == InstallStatus::ValueOutOfRange);
}

// --------------------------------------------------------------- atomicity --

TEST(a_rejected_document_leaves_the_defaults_untouched)
{
    // Applying half a corrupt document would leave the device running a
    // mixture nobody chose, which is harder to diagnose than either.
    pinled_v1_InstallConfig in = blank();
    in.has_geometry = true;
    in.geometry.num_modules = 4;   // valid, and applied first in the code
    in.has_scan = true;
    in.scan.spi_mode = 7;          // invalid, rejected later

    MachineConfig out = fallback();
    out.num_modules = 99; // proves `out` is written, not merely left alone
    CHECK(install_to_machine(in, out, fallback()) == InstallStatus::ValueOutOfRange);
    CHECK_EQ(out.num_modules, static_cast<size_t>(99));
}

// ------------------------------------------------- the reverse projection --

TEST(a_running_config_round_trips_through_a_document)
{
    // What GET /api/v1/config returns when nothing is stored. It has to be
    // accurate about the fields it carries, because the UI will show it as
    // "what this device is doing" and may well PUT it straight back.
    const MachineConfig before = fallback();

    pinled_v1_InstallConfig doc = blank();
    machine_to_install(before, nullptr, 0, doc);

    MachineConfig after{};
    CHECK(install_to_machine(doc, after, MachineConfig{}) == InstallStatus::Ok);

    CHECK_EQ(after.num_modules, before.num_modules);
    CHECK_EQ(after.channels_per_module, before.channels_per_module);
    CHECK_EQ(after.led_count, before.led_count);
    CHECK_EQ(after.clk_pin, before.clk_pin);
    CHECK_EQ(after.pl_pin, before.pl_pin);
    CHECK_EQ(after.data_pin, before.data_pin);
    CHECK_EQ(after.led_pin, before.led_pin);
    CHECK_EQ(after.spi_hz, before.spi_hz);
    CHECK_EQ(after.spi_mode, before.spi_mode);
    CHECK_EQ(after.active_low, before.active_low);
    CHECK_EQ(after.pl_from_cs, before.pl_from_cs);
    CHECK_EQ(after.refresh_hz, before.refresh_hz);
    CHECK(near(after.sample_rate_hz, before.sample_rate_hz));
    CHECK(near(after.gamma, before.gamma));
    CHECK(near(after.attack_ms, before.attack_ms));
    CHECK(near(after.decay_ms, before.decay_ms));
}

TEST(a_not_fitted_pin_survives_the_round_trip)
{
    // -1 through an int32 and back. If this became 0 the document would claim
    // /PL is on GPIO 0, and a UI that PUT it back would break the chain.
    MachineConfig before = fallback();
    before.pl_pin = kPinNotFitted;

    pinled_v1_InstallConfig doc = blank();
    machine_to_install(before, nullptr, 0, doc);

    MachineConfig after{};
    CHECK(install_to_machine(doc, after, MachineConfig{}) == InstallStatus::Ok);
    CHECK_EQ(after.pl_pin, kPinNotFitted);
}

TEST(the_strip_byte_order_survives_the_round_trip)
{
    MachineConfig before = fallback();
    before.color_order = ColorOrder::BRG;

    pinled_v1_InstallConfig doc = blank();
    machine_to_install(before, nullptr, 0, doc);

    MachineConfig after{};
    CHECK(install_to_machine(doc, after, MachineConfig{}) == InstallStatus::Ok);
    CHECK(after.color_order == ColorOrder::BRG);
}

TEST(wiring_is_emitted_for_bound_or_mapped_channels_only)
{
    ChannelConfig ch[8]{};
    ch[0].lamp = 11; ch[0].led_index = 0;   // bound and mapped
    ch[1].lamp = 0;  ch[1].led_index = 3;   // mapped only
    ch[2].lamp = 7;  ch[2].led_index = -1;  // bound only
    // 3..7 left neither, and must not appear.

    pinled_v1_InstallConfig doc = blank();
    machine_to_install(fallback(), ch, 8, doc);

    CHECK_EQ(static_cast<int>(doc.wiring_count), 3);
    CHECK_EQ(doc.wiring[0].channel, static_cast<uint32_t>(0));
    CHECK_EQ(doc.wiring[0].lamp, static_cast<uint32_t>(11));
    CHECK_EQ(doc.wiring[1].channel, static_cast<uint32_t>(1));
    CHECK_EQ(doc.wiring[1].led_index, 3);
    CHECK_EQ(doc.wiring[2].channel, static_cast<uint32_t>(2));
    CHECK_EQ(doc.wiring[2].led_index, -1);
}

TEST(a_wiring_round_trip_lands_on_the_same_channels)
{
    // The join is the part most worth checking: channel -> lamp -> LED has to
    // survive being written out and read back, or a UI that reads and rewrites
    // the running config would silently remap the playfield.
    ChannelConfig before[32]{};
    before[0].lamp = 11; before[0].led_index = 0;
    before[4].lamp = 2;  before[4].led_index = 1;
    before[31].lamp = 99; before[31].led_index = 15;

    pinled_v1_InstallConfig doc = blank();
    machine_to_install(fallback(), before, 32, doc);

    ChannelConfig after[32]{};
    size_t n = 0;
    const pinled_v1_MachineProfile empty = pinled_v1_MachineProfile_init_zero;
    CHECK(resolve(empty, doc, after, 32, &n) == ResolveStatus::Ok);
    CHECK_EQ(n, static_cast<size_t>(32));

    CHECK_EQ(after[0].lamp, static_cast<uint16_t>(11));
    CHECK_EQ(after[0].led_index, static_cast<int16_t>(0));
    CHECK_EQ(after[4].lamp, static_cast<uint16_t>(2));
    CHECK_EQ(after[4].led_index, static_cast<int16_t>(1));
    CHECK_EQ(after[31].lamp, static_cast<uint16_t>(99));
    CHECK_EQ(after[31].led_index, static_cast<int16_t>(15));
    CHECK_EQ(after[7].lamp, static_cast<uint16_t>(0));
    CHECK_EQ(after[7].led_index, static_cast<int16_t>(-1));
}

TEST(the_reverse_projection_does_not_invent_a_gain)
{
    // MachineConfig has no device-wide gain. Emitting 1000 would look like
    // someone chose unity; 0 means inherit, which is the truth.
    pinled_v1_InstallConfig doc = blank();
    machine_to_install(fallback(), nullptr, 0, doc);
    CHECK(doc.has_filament);
    CHECK_EQ(doc.filament.gain_permille, static_cast<uint32_t>(0));
}

int main() { return ooe::test::run_all(); }
