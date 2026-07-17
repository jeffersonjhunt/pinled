/**
 * @file filament.cpp
 * @brief Fixed-point incandescent-filament emulator implementation.
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 */

#include "filament.h"

#include <cmath>
#include <cstring>
#include <new>

#include "esp_log.h"

namespace ooe::pinled
{
    static const char *TAG = "filament";

    Filament::~Filament()
    {
        deinit();
    }

    int32_t Filament::coeff_q16(float tau_ms, float fs_hz)
    {
        // One-pole smoothing coefficient: k = 1 - exp(-dt / tau), dt = 1/fs.
        // Guard against degenerate config (instant response).
        if (tau_ms <= 0.0f || fs_hz <= 0.0f)
            return Q16_ONE;
        const float tau_s = tau_ms * 1e-3f;
        const float k = 1.0f - std::exp(-1.0f / (tau_s * fs_hz));
        int32_t q = static_cast<int32_t>(k * static_cast<float>(Q16_ONE) + 0.5f);
        if (q < 1)
            q = 1;
        if (q > Q16_ONE)
            q = Q16_ONE;
        return q;
    }

    esp_err_t Filament::init(size_t num_channels, float sample_rate_hz)
    {
        if (num_channels == 0 || sample_rate_hz <= 0.0f)
            return ESP_ERR_INVALID_ARG;

        deinit();

        level_ = new (std::nothrow) int32_t[num_channels]();
        k_attack_ = new (std::nothrow) int32_t[num_channels];
        k_decay_ = new (std::nothrow) int32_t[num_channels];
        gain_q16_ = new (std::nothrow) int32_t[num_channels];
        if (!level_ || !k_attack_ || !k_decay_ || !gain_q16_)
        {
            ESP_LOGE(TAG, "alloc failed for %u channels", (unsigned)num_channels);
            deinit();
            return ESP_ERR_NO_MEM;
        }

        num_channels_ = num_channels;
        sample_rate_hz_ = sample_rate_hz;

        set_gamma(2.2f);
        const esp_err_t err = set_params_all(FilamentParams{});
        ESP_LOGI(TAG, "init: %u channels @ %.0f Hz", (unsigned)num_channels, sample_rate_hz);
        return err;
    }

    void Filament::deinit()
    {
        delete[] level_;
        delete[] k_attack_;
        delete[] k_decay_;
        delete[] gain_q16_;
        level_ = k_attack_ = k_decay_ = gain_q16_ = nullptr;
        num_channels_ = 0;
    }

    esp_err_t Filament::set_params(size_t ch, const FilamentParams &p)
    {
        if (ch >= num_channels_)
            return ESP_ERR_INVALID_ARG;
        k_attack_[ch] = coeff_q16(p.attack_ms, sample_rate_hz_);
        k_decay_[ch] = coeff_q16(p.decay_ms, sample_rate_hz_);
        int32_t g = static_cast<int32_t>(p.gain * static_cast<float>(Q16_ONE) + 0.5f);
        if (g < 0)
            g = 0;
        gain_q16_[ch] = g;
        return ESP_OK;
    }

    esp_err_t Filament::set_params_all(const FilamentParams &p)
    {
        for (size_t ch = 0; ch < num_channels_; ++ch)
        {
            const esp_err_t err = set_params(ch, p);
            if (err != ESP_OK)
                return err;
        }
        return ESP_OK;
    }

    void Filament::set_gamma(float gamma)
    {
        if (gamma <= 0.0f)
            gamma = 1.0f;
        for (int i = 0; i < 256; ++i)
        {
            const float x = static_cast<float>(i) / 255.0f;
            const float y = std::pow(x, gamma);
            int v = static_cast<int>(y * 255.0f + 0.5f);
            if (v < 0)
                v = 0;
            if (v > 255)
                v = 255;
            gamma_lut_[i] = static_cast<uint8_t>(v);
        }
    }

    void Filament::snapshot(uint8_t *out, size_t n) const
    {
        if (!out)
            return;
        const size_t count = n < num_channels_ ? n : num_channels_;
        for (size_t ch = 0; ch < count; ++ch)
            out[ch] = level(ch);
    }
} // namespace ooe::pinled
