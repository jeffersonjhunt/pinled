#pragma once

/**
 * @file filament.h
 * @author Jefferson J. Hunt (jefferson@oneoffendeavors.com)
 * @brief Per-channel incandescent-filament emulator.
 * @version 0.2.0
 * @date 2026-07-16
 *
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * An incandescent bulb is a thermal low-pass filter: its brightness is the
 * time-average of the power delivered, with a time constant of ~20-50 ms.
 * Every pinball lamp-drive scheme (steady DC, strobed matrix, phase-chopped GI)
 * was designed to look correct through that filter. This component *is* that
 * filter: a fixed-point leaky integrator per channel that reconstructs the
 * brightness the original bulb would have shown.
 *
 * Hot-path methods (update / update_duty / level) do no allocation and no
 * floating point. Coefficients and the gamma LUT are computed once, off the
 * hot path, whenever parameters change.
 */

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace ooe::pinled
{
    /// Per-channel tuning. Attack/decay are thermal time constants; gain
    /// normalizes duty (e.g. a ~1/8-duty matrix lamp uses gain ~8 to reach full
    /// brightness, while a dimmed GI channel uses gain 1 so brightness tracks
    /// the conduction angle).
    struct FilamentParams
    {
        float attack_ms{30.0f}; ///< rise time constant (ms)
        float decay_ms{40.0f};  ///< fall time constant (ms)
        float gain{1.0f};       ///< duty normalization multiplier at output
    };

    class Filament
    {
    public:
        static constexpr int32_t Q16_ONE = 1 << 16;

        Filament() = default;
        ~Filament();

        /**
         * @brief Allocate channel state and precompute default coefficients.
         * @param num_channels total sensed channels across all modules
         * @param sample_rate_hz per-channel sample rate (drives tau -> k)
         */
        esp_err_t init(size_t num_channels, float sample_rate_hz);
        void deinit();

        /// Set one channel's parameters (recomputes that channel's coeffs).
        esp_err_t set_params(size_t ch, const FilamentParams &p);
        /// Set every channel to the same parameters.
        esp_err_t set_params_all(const FilamentParams &p);
        /// Rebuild the shared gamma LUT (gamma 1.0 = linear).
        void set_gamma(float gamma);

        /// Hot path: advance one channel by one boolean sample (on/off).
        inline void update(size_t ch, bool active)
        {
            int32_t lvl = level_[ch];
            const int32_t target = active ? Q16_ONE : 0;
            if (target > lvl)
                lvl += static_cast<int32_t>((static_cast<int64_t>(target - lvl) * k_attack_[ch]) >> 16);
            else
                lvl -= static_cast<int32_t>((static_cast<int64_t>(lvl - target) * k_decay_[ch]) >> 16);
            level_[ch] = lvl;
        }

        /// Hot path: advance one channel toward a measured duty (Q16, 0..65536).
        inline void update_duty(size_t ch, int32_t duty_q16)
        {
            int32_t lvl = level_[ch];
            if (duty_q16 > lvl)
                lvl += static_cast<int32_t>((static_cast<int64_t>(duty_q16 - lvl) * k_attack_[ch]) >> 16);
            else
                lvl -= static_cast<int32_t>((static_cast<int64_t>(lvl - duty_q16) * k_decay_[ch]) >> 16);
            level_[ch] = lvl;
        }

        /// Gamma-corrected 0..255 brightness for one channel (gain applied).
        inline uint8_t level(size_t ch) const
        {
            int32_t g = static_cast<int32_t>((static_cast<int64_t>(level_[ch]) * gain_q16_[ch]) >> 16);
            if (g > Q16_ONE)
                g = Q16_ONE;
            int idx = g >> 8; // 0..256
            if (idx > 255)
                idx = 255;
            return gamma_lut_[idx];
        }

        /// Copy 0..255 levels for all channels into out[0..n) for the renderer.
        void snapshot(uint8_t *out, size_t n) const;

        size_t channels() const { return num_channels_; }

    private:
        static int32_t coeff_q16(float tau_ms, float fs_hz);

        size_t num_channels_{0};
        float sample_rate_hz_{0.0f};

        int32_t *level_{nullptr};    ///< Q16 integrator state per channel
        int32_t *k_attack_{nullptr}; ///< Q16 attack coefficient per channel
        int32_t *k_decay_{nullptr};  ///< Q16 decay coefficient per channel
        int32_t *gain_q16_{nullptr}; ///< Q16 output gain per channel
        uint8_t gamma_lut_[256]{};   ///< shared gamma LUT (level>>8 -> 0..255)
    };
} // namespace ooe::pinled
