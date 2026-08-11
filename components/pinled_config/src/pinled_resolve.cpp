/**
 * @file pinled_resolve.cpp
 * @brief Projection of the two configuration documents onto per-channel state.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "pinled_resolve.h"

namespace ooe::pinled
{
    namespace
    {
        /// FR-SCAN-3: 8 modules x 16 channels. Also `pinled.options`' max_count.
        constexpr size_t kMaxChannels = 128;

        constexpr uint32_t kMaxByte = 255;
        constexpr uint32_t kMaxU16 = 65535;
        constexpr int32_t kMaxI16 = 32767;

        // Bounds for install_to_machine. Not tuning knobs — each one is the
        // point past which a value cannot be a real setting, so the only thing
        // it can be is a corrupt document. Fs is clamped to what the hardware
        // measurably sustains at boot anyway (FR-SCAN-9), so this only has to
        // catch nonsense.
        constexpr int32_t kMaxPinNumber = 63;   ///< widest GPIO numbering any ESP32 variant uses
        constexpr uint32_t kMaxSampleRateHz = 100000;
        constexpr uint32_t kMinSpiHz = 100000;
        constexpr uint32_t kMaxSpiHz = 80000000;
        constexpr uint32_t kMaxRefreshHz = 1000;
        constexpr uint32_t kMinGammaX100 = 10;  ///< 0.10
        constexpr uint32_t kMaxGammaX100 = 1000; ///< 10.0

        // The record is read every frame by the render path, so its size is a
        // design constraint rather than an accident. 14 bytes packs with no
        // padding; a field added carelessly would silently cost 128 x 2.
        static_assert(sizeof(ChannelConfig) == 16, "ChannelConfig grew — check the layout");

        // The runtime enum and the wire enum are two lists that must agree.
        // Asserting each pairing means a value added to one and not the other
        // fails to compile, rather than silently classifying a lamp wrongly.
        static_assert(static_cast<int>(DriveClass::UNKNOWN) ==
                          pinled_v1_DriveClass_DRIVE_CLASS_UNSPECIFIED, "");
        static_assert(static_cast<int>(DriveClass::OFF) ==
                          pinled_v1_DriveClass_DRIVE_CLASS_OFF, "");
        static_assert(static_cast<int>(DriveClass::STEADY) ==
                          pinled_v1_DriveClass_DRIVE_CLASS_STEADY, "");
        static_assert(static_cast<int>(DriveClass::MATRIX) ==
                          pinled_v1_DriveClass_DRIVE_CLASS_MATRIX, "");
        static_assert(static_cast<int>(DriveClass::AC_STEADY) ==
                          pinled_v1_DriveClass_DRIVE_CLASS_AC_STEADY, "");
        static_assert(static_cast<int>(DriveClass::AC_DIMMED) ==
                          pinled_v1_DriveClass_DRIVE_CLASS_AC_DIMMED, "");

        /// Locate a lamp's profile entry, or null. Linear by design — see the
        /// complexity note in the header.
        const pinled_v1_LampEntry *find_lamp(const pinled_v1_MachineProfile &p, uint16_t lamp)
        {
            for (pb_size_t i = 0; i < p.lamps_count; ++i)
                if (p.lamps[i].lamp == lamp)
                    return &p.lamps[i];
            return nullptr;
        }

        /// A per-lamp zero inherits; anything else wins.
        uint16_t inherit(uint32_t entry, uint16_t fallback)
        {
            return entry != 0 ? static_cast<uint16_t>(entry) : fallback;
        }
    } // namespace

    DriveClass drive_class_from_proto(pinled_v1_DriveClass c)
    {
        switch (c)
        {
        case pinled_v1_DriveClass_DRIVE_CLASS_OFF:       return DriveClass::OFF;
        case pinled_v1_DriveClass_DRIVE_CLASS_STEADY:    return DriveClass::STEADY;
        case pinled_v1_DriveClass_DRIVE_CLASS_MATRIX:    return DriveClass::MATRIX;
        case pinled_v1_DriveClass_DRIVE_CLASS_AC_STEADY: return DriveClass::AC_STEADY;
        case pinled_v1_DriveClass_DRIVE_CLASS_AC_DIMMED: return DriveClass::AC_DIMMED;
        case pinled_v1_DriveClass_DRIVE_CLASS_UNSPECIFIED:
        default:
            // A value from a newer schema than this build understands must mean
            // "leave it to the profiler", not "guess". Unknown enum values are
            // preserved on the wire but cannot be honoured here.
            return DriveClass::UNKNOWN;
        }
    }

    pinled_v1_DriveClass drive_class_to_proto(DriveClass c)
    {
        switch (c)
        {
        case DriveClass::OFF:       return pinled_v1_DriveClass_DRIVE_CLASS_OFF;
        case DriveClass::STEADY:    return pinled_v1_DriveClass_DRIVE_CLASS_STEADY;
        case DriveClass::MATRIX:    return pinled_v1_DriveClass_DRIVE_CLASS_MATRIX;
        case DriveClass::AC_STEADY: return pinled_v1_DriveClass_DRIVE_CLASS_AC_STEADY;
        case DriveClass::AC_DIMMED: return pinled_v1_DriveClass_DRIVE_CLASS_AC_DIMMED;
        case DriveClass::UNKNOWN:   break;
        }
        return pinled_v1_DriveClass_DRIVE_CLASS_UNSPECIFIED;
    }

    const char *install_status_str(InstallStatus s)
    {
        switch (s)
        {
        case InstallStatus::Ok:               return "ok";
        case InstallStatus::GeometryTooLarge: return "geometry too large";
        case InstallStatus::PinOutOfRange:    return "pin out of range";
        case InstallStatus::ValueOutOfRange:  return "value out of range";
        }
        return "unknown";
    }

    InstallStatus install_to_machine(const pinled_v1_InstallConfig &install,
                                     MachineConfig &out,
                                     const MachineConfig &fallback)
    {
        // Everything is validated against locals first; `out` is written once
        // at the end. A document rejected halfway must not leave the caller
        // running a mixture of its defaults and a corrupt file.
        MachineConfig m = fallback;

        // --- geometry ---
        if (install.has_geometry)
        {
            const pinled_v1_Geometry &g = install.geometry;
            if (g.num_modules != 0)
                m.num_modules = g.num_modules;
            if (g.channels_per_module != 0)
                m.channels_per_module = g.channels_per_module;
            if (g.led_count != 0)
            {
                if (g.led_count > kMaxLedCount)
                    return InstallStatus::GeometryTooLarge;
                m.led_count = g.led_count;
            }
            if (m.num_modules * m.channels_per_module > kMaxChannels)
                return InstallStatus::GeometryTooLarge;
        }

        // --- pins ---
        if (install.has_pins)
        {
            const pinled_v1_Pins &p = install.pins;
            const bool all_zero = (p.clk == 0 && p.pl == 0 && p.data == 0 && p.led == 0);
            if (!all_zero) // see the header: four signals cannot share GPIO 0
            {
                const int32_t pins[] = {p.clk, p.pl, p.data, p.led};
                for (int32_t v : pins)
                    if (v < kPinNotFitted || v > kMaxPinNumber)
                        return InstallStatus::PinOutOfRange;
                m.clk_pin = p.clk;
                m.pl_pin = p.pl;
                m.data_pin = p.data;
                m.led_pin = p.led;
            }
        }

        // --- scan ---
        if (install.has_scan)
        {
            const pinled_v1_ScanConfig &s = install.scan;
            if (s.sample_rate_hz != 0)
            {
                if (s.sample_rate_hz > kMaxSampleRateHz)
                    return InstallStatus::ValueOutOfRange;
                m.sample_rate_hz = static_cast<float>(s.sample_rate_hz);
            }
            if (s.spi_hz != 0)
            {
                if (s.spi_hz < kMinSpiHz || s.spi_hz > kMaxSpiHz)
                    return InstallStatus::ValueOutOfRange;
                m.spi_hz = static_cast<int32_t>(s.spi_hz);
            }
            // 0 is a real SPI mode, so this is taken literally rather than
            // inherited — which makes the range check the only guard there is.
            if (s.spi_mode > 3)
                return InstallStatus::ValueOutOfRange;
            m.spi_mode = static_cast<uint8_t>(s.spi_mode);

            // Booleans have no presence either. A present ScanConfig states
            // both of these; that is what "present" means for a flag.
            m.active_low = s.active_low;
            m.pl_from_cs = s.pl_from_cs;
        }

        // --- render ---
        if (install.has_render)
        {
            const pinled_v1_RenderConfig &r = install.render;
            if (r.refresh_hz != 0)
            {
                if (r.refresh_hz > kMaxRefreshHz)
                    return InstallStatus::ValueOutOfRange;
                m.refresh_hz = r.refresh_hz;
            }
            if (r.gamma_x100 != 0)
            {
                if (r.gamma_x100 < kMinGammaX100 || r.gamma_x100 > kMaxGammaX100)
                    return InstallStatus::ValueOutOfRange;
                m.gamma = static_cast<float>(r.gamma_x100) / 100.0f;
            }
            // brightness_cap is carried in the schema but the render path has
            // no PSU budget yet (lamp_map.cpp TODO), so it is deliberately not
            // projected. Reading it into a field nothing honours would look
            // like a feature.
        }

        // --- filament defaults ---
        if (install.has_filament)
        {
            const pinled_v1_FilamentDefaults &f = install.filament;
            if (f.attack_ms > kMaxU16 || f.decay_ms > kMaxU16)
                return InstallStatus::ValueOutOfRange;
            if (f.attack_ms != 0)
                m.attack_ms = static_cast<float>(f.attack_ms);
            if (f.decay_ms != 0)
                m.decay_ms = static_cast<float>(f.decay_ms);
            // gain_permille is per-channel and reaches ChannelConfig through
            // resolve(); MachineConfig has no device-wide gain to put it in.
        }

        out = m;
        return InstallStatus::Ok;
    }

    const char *resolve_status_str(ResolveStatus s)
    {
        switch (s)
        {
        case ResolveStatus::Ok:                return "ok";
        case ResolveStatus::NullOutput:        return "null output";
        case ResolveStatus::GeometryTooLarge:  return "geometry too large";
        case ResolveStatus::ChannelOutOfRange: return "channel out of range";
        case ResolveStatus::DuplicateChannel:  return "duplicate channel";
        case ResolveStatus::LedOutOfRange:     return "led index out of range";
        case ResolveStatus::ValueOutOfRange:   return "value out of range";
        }
        return "unknown";
    }

    ResolveStatus resolve(const pinled_v1_MachineProfile &profile,
                          const pinled_v1_InstallConfig &install,
                          ChannelConfig *out, size_t out_cap,
                          size_t *channels_out,
                          const ResolveDefaults &fallback)
    {
        if (channels_out != nullptr)
            *channels_out = 0;

        const size_t total =
            install.has_geometry
                ? static_cast<size_t>(install.geometry.num_modules) *
                      static_cast<size_t>(install.geometry.channels_per_module)
                : 0;

        if (total > kMaxChannels)
            return ResolveStatus::GeometryTooLarge;
        if (total > out_cap)
            return ResolveStatus::GeometryTooLarge;
        if (out == nullptr && total > 0)
            return ResolveStatus::NullOutput;

        // Defaults the per-lamp zeros inherit from. A zero in the install
        // config inherits in turn, from the compiled-in fallback — so "unset"
        // is expressible at every level without a sentinel.
        if (install.has_filament &&
            (install.filament.attack_ms > kMaxU16 ||
             install.filament.decay_ms > kMaxU16 ||
             install.filament.gain_permille > kMaxU16))
            return ResolveStatus::ValueOutOfRange;

        const uint16_t d_attack =
            install.has_filament ? inherit(install.filament.attack_ms, fallback.attack_ms)
                                 : fallback.attack_ms;
        const uint16_t d_decay =
            install.has_filament ? inherit(install.filament.decay_ms, fallback.decay_ms)
                                 : fallback.decay_ms;
        const uint16_t d_gain =
            install.has_filament ? inherit(install.filament.gain_permille, fallback.gain_permille)
                                 : fallback.gain_permille;

        // Every slot is written, including unbound ones, so a caller reusing a
        // buffer across a reconfigure never reads a stale entry. Unbound
        // channels still carry resolved defaults: binding one later must not
        // change how it behaves beyond gaining a lamp.
        for (size_t i = 0; i < total; ++i)
        {
            out[i] = ChannelConfig{};
            out[i].r = fallback.r;
            out[i].g = fallback.g;
            out[i].b = fallback.b;
            out[i].attack_ms = d_attack;
            out[i].decay_ms = d_decay;
            out[i].gain_permille = d_gain;
        }

        // --- the join: channel -> lamp, from the install ---

        const uint32_t led_count =
            install.has_geometry ? install.geometry.led_count : 0;

        // An explicit claimed-set rather than inferring from the record. A
        // wiring entry may legitimately carry lamp 0 and led_index -1 (a
        // placeholder for a channel someone has started on), and inferring
        // "claimed" from those fields would silently accept a duplicate pair of
        // them — the one duplicate the check would miss.
        uint8_t claimed[kMaxChannels / 8] = {};

        for (pb_size_t i = 0; i < install.wiring_count; ++i)
        {
            const pinled_v1_WiringEntry &w = install.wiring[i];

            if (w.channel >= total)
                return ResolveStatus::ChannelOutOfRange;
            if (w.lamp > kMaxU16)
                return ResolveStatus::ValueOutOfRange;
            if (w.led_index < -1 || w.led_index > kMaxI16)
                return ResolveStatus::LedOutOfRange;
            if (w.led_index >= 0 && static_cast<uint32_t>(w.led_index) >= led_count)
                return ResolveStatus::LedOutOfRange;

            const size_t byte = w.channel / 8;
            const uint8_t bit = static_cast<uint8_t>(1u << (w.channel % 8));
            if (claimed[byte] & bit)
                return ResolveStatus::DuplicateChannel;
            claimed[byte] = static_cast<uint8_t>(claimed[byte] | bit);

            ChannelConfig &c = out[w.channel];
            c.lamp = static_cast<uint16_t>(w.lamp);
            c.led_index = static_cast<int16_t>(w.led_index);
        }

        // --- styling: lamp -> profile entry ---

        for (size_t i = 0; i < total; ++i)
        {
            ChannelConfig &c = out[i];
            if (c.lamp == 0)
                continue; // wired but unassigned, or not wired at all

            const pinled_v1_LampEntry *e = find_lamp(profile, c.lamp);
            if (e == nullptr)
                continue; // bound but unstyled — legitimate, keeps the defaults

            // Validate everything before writing anything, so a rejected
            // document leaves out[] in one state rather than half-updated.
            if (e->has_color &&
                (e->color.r > kMaxByte || e->color.g > kMaxByte || e->color.b > kMaxByte))
                return ResolveStatus::ValueOutOfRange;
            if (e->attack_ms > kMaxU16 || e->decay_ms > kMaxU16 ||
                e->gain_permille > kMaxU16)
                return ResolveStatus::ValueOutOfRange;

            if (e->has_color)
            {
                c.r = static_cast<uint8_t>(e->color.r);
                c.g = static_cast<uint8_t>(e->color.g);
                c.b = static_cast<uint8_t>(e->color.b);
                c.flags = static_cast<uint8_t>(c.flags | CH_COLOR_SET);
            }

            c.class_lock = drive_class_from_proto(e->class_lock);
            c.attack_ms = inherit(e->attack_ms, d_attack);
            c.decay_ms = inherit(e->decay_ms, d_decay);
            c.gain_permille = inherit(e->gain_permille, d_gain);

            // Record what a person actually chose. Settling the inherit-markers
            // above is what lets everything downstream stay ignorant of the
            // convention — but it also erases the difference between "30 ms
            // because someone asked for it" and "30 ms because that is the
            // default", and the profiler must not overwrite the former.
            if (e->attack_ms != 0)
                c.flags = static_cast<uint8_t>(c.flags | CH_ATTACK_SET);
            if (e->decay_ms != 0)
                c.flags = static_cast<uint8_t>(c.flags | CH_DECAY_SET);
            if (e->gain_permille != 0)
                c.flags = static_cast<uint8_t>(c.flags | CH_GAIN_SET);
        }

        if (channels_out != nullptr)
            *channels_out = total;
        return ResolveStatus::Ok;
    }
} // namespace ooe::pinled
