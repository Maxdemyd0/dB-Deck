#pragma once
// Generated sine source. Used for diagnostics and verification so a user can
// confirm routing without any capture device. Always labelled as generated.

#include <atomic>
#include <cmath>
#include <numbers>

#include "audio/PullSource.h"

namespace dbdeck {

class ToneGenerator final : public PullSource {
public:
    ToneGenerator(double rate, float hz, float amplitude)
        : rate_(rate), hz_(hz), amp_(amplitude) {}

    void setFrequency(float hz) { hz_.store(hz, std::memory_order_relaxed); }

    void pull(float* out, size_t frames) override {
        const double inc = 2.0 * std::numbers::pi * hz_.load(std::memory_order_relaxed) / rate_;
        const float a = amp_;
        for (size_t i = 0; i < frames; ++i) {
            const float v = a * static_cast<float>(std::sin(phase_));
            out[i * 2] = v;
            out[i * 2 + 1] = v;
            phase_ += inc;
            if (phase_ > 2.0 * std::numbers::pi) phase_ -= 2.0 * std::numbers::pi;
        }
    }

private:
    double rate_;
    std::atomic<float> hz_;
    float amp_;
    double phase_ = 0.0;
};

}  // namespace dbdeck
