/**
 * @file profiler.cpp
 * @brief Drive-scheme classifier (first-cut skeleton).
 * @copyright Copyright (c) 2024-2026 Jefferson J. Hunt (MIT)
 *
 * TODO(v1): inter-edge interval histogram for period_est_ms; distinguish
 * AC_STEADY vs AC_DIMMED by conduction-angle variance; confidence from window
 * length and edge regularity. The scaffolding (duty + edge counting, param
 * mapping) is complete and unit-testable now.
 */

#include "profiler.h"

#include <cmath>
#include <new>

#include "esp_log.h"

namespace ooe::pinled
{
    static const char *TAG = "profiler";

    esp_err_t Profiler::init(size_t num_channels, float sample_rate_hz)
    {
        if (num_channels == 0 || sample_rate_hz <= 0.0f)
            return ESP_ERR_INVALID_ARG;
        deinit();
        high_count_ = new (std::nothrow) uint32_t[num_channels]();
        edges_ = new (std::nothrow) uint32_t[num_channels]();
        last_ = new (std::nothrow) uint8_t[num_channels]();
        if (!high_count_ || !edges_ || !last_)
        {
            deinit();
            return ESP_ERR_NO_MEM;
        }
        num_channels_ = num_channels;
        sample_rate_hz_ = sample_rate_hz;
        arm();
        ESP_LOGI(TAG, "init: %u channels @ %.0f Hz", (unsigned)num_channels, sample_rate_hz);
        return ESP_OK;
    }

    void Profiler::deinit()
    {
        delete[] high_count_;
        delete[] edges_;
        delete[] last_;
        high_count_ = edges_ = nullptr;
        last_ = nullptr;
        num_channels_ = 0;
    }

    void Profiler::arm()
    {
        frames_ = 0;
        for (size_t ch = 0; ch < num_channels_; ++ch)
        {
            high_count_[ch] = 0;
            edges_[ch] = 0;
            last_[ch] = 0;
        }
    }

    void Profiler::observe(const bool *samples, size_t n)
    {
        if (!samples)
            return;
        // Don't count a transition against the reset state on the first frame,
        // or a genuinely steady channel logs a phantom edge and misclassifies.
        const bool first = (frames_ == 0);
        const size_t count = n < num_channels_ ? n : num_channels_;
        for (size_t ch = 0; ch < count; ++ch)
        {
            const uint8_t s = samples[ch] ? 1 : 0;
            if (s)
                ++high_count_[ch];
            if (!first && s != last_[ch])
                ++edges_[ch];
            last_[ch] = s;
        }
        ++frames_;
    }

    FilamentParams Profiler::params_for(const ChannelProfile &p)
    {
        FilamentParams fp{}; // defaults: 30/40 ms, gain 1
        switch (p.klass)
        {
        case DriveClass::STEADY:
        case DriveClass::AC_STEADY:
            fp.gain = 1.0f;
            break;
        case DriveClass::AC_DIMMED:
            // Track conduction angle: unity gain, slightly faster follow.
            fp.gain = 1.0f;
            fp.attack_ms = 20.0f;
            fp.decay_ms = 30.0f;
            break;
        case DriveClass::MATRIX:
        {
            // Normalize low duty up to full brightness; short attack to catch
            // the column strobe, longer decay to hold between bursts.
            const float duty = p.duty_q16 > 0 ? static_cast<float>(p.duty_q16) / 65536.0f : 0.125f;
            float g = duty > 0.0f ? 1.0f / duty : 8.0f;
            if (g > 16.0f)
                g = 16.0f; // clamp
            fp.gain = g;
            fp.attack_ms = 15.0f;
            fp.decay_ms = 45.0f;
            break;
        }
        case DriveClass::OFF:
        case DriveClass::UNKNOWN:
        default:
            fp.gain = 1.0f;
            break;
        }
        return fp;
    }

    esp_err_t Profiler::classify(ChannelProfile *profiles, FilamentParams *params, size_t n)
    {
        if (!profiles || !params || n < num_channels_)
            return ESP_ERR_INVALID_ARG;
        if (frames_ == 0)
            return ESP_ERR_INVALID_STATE;

        for (size_t ch = 0; ch < num_channels_; ++ch)
        {
            ChannelProfile &p = profiles[ch];
            if (p.locked)
            {
                params[ch] = params_for(p);
                continue;
            }

            const float duty = static_cast<float>(high_count_[ch]) / static_cast<float>(frames_);
            const uint32_t edges = edges_[ch];
            p.duty_q16 = static_cast<int32_t>(duty * 65536.0f + 0.5f);
            p.period_est_ms = 0.0f; // TODO(v1)

            // First-cut decision tree (period estimation refines this in v1).
            if (edges == 0)
            {
                p.klass = duty > 0.5f ? DriveClass::STEADY : DriveClass::OFF;
                p.confidence = 200;
            }
            else if (duty < 0.35f)
            {
                // Low duty with activity looks like a strobed matrix lamp.
                p.klass = DriveClass::MATRIX;
                p.confidence = 140;
            }
            else
            {
                // Mid/high duty with edges: AC-ish. Dimmed vs steady needs the
                // conduction-angle test (v1); default to AC_STEADY for now.
                p.klass = DriveClass::AC_STEADY;
                p.confidence = 100;
            }

            params[ch] = params_for(p);
        }

        ESP_LOGI(TAG, "classified %u channels over %u frames",
                 (unsigned)num_channels_, (unsigned)frames_);
        return ESP_OK;
    }
} // namespace ooe::pinled
