#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>

namespace dbdeck {

inline float dbToLinear(float db) { return db <= -144.0f ? 0.0f : std::pow(10.0f, db / 20.0f); }
inline float linearToDb(float lin) { return lin <= 1e-8f ? -160.0f : 20.0f * std::log10(lin); }

// Fader taper used for managed (engine-processed) audio. A fader position of
// 1.0 is unity gain; 0.5 is about -12 dB; 0 is silence. Windows session volume
// (application channels) uses the raw fader scalar instead, matching the
// Windows Volume Mixer.
inline float faderToGain(float position) {
    const float p = std::clamp(position, 0.0f, 1.0f);
    return p * p;
}

// Balance law: center is unity on both sides; moving away attenuates the
// opposite side linearly to silence at the extremes.
inline void balanceGains(float balance, float& l, float& r) {
    const float b = std::clamp(balance, -1.0f, 1.0f);
    l = b > 0.0f ? 1.0f - b : 1.0f;
    r = b < 0.0f ? 1.0f + b : 1.0f;
}

// One-pole parameter smoother, run per sample on the audio thread. The
// target is set from any thread through an atomic; `current` is audio-only.
class SmoothedParam {
public:
    explicit SmoothedParam(float initial = 0.0f) : target_(initial), current_(initial) {}

    void setTarget(float v) { target_.store(v, std::memory_order_relaxed); }
    float target() const { return target_.load(std::memory_order_relaxed); }

    // Audio thread only.
    void snap() { current_ = target_.load(std::memory_order_relaxed); }
    void resetTo(float v) { current_ = v; }
    float current() const { return current_; }

    // Advances one sample; coeff from smoothingCoeff().
    float next(float coeff) {
        const float t = target_.load(std::memory_order_relaxed);
        current_ += (t - current_) * coeff;
        if (std::fabs(t - current_) < 1e-6f) current_ = t;
        return current_;
    }

    bool settled() const { return current_ == target_.load(std::memory_order_relaxed); }

private:
    std::atomic<float> target_;
    float current_;
};

// Coefficient for a ~`ms` time constant at `rate` Hz.
inline float smoothingCoeff(float ms, double rate) {
    return 1.0f - std::exp(-1.0f / (ms * 0.001f * static_cast<float>(rate)));
}

// Multiplies a stereo buffer by a smoothed gain (identical on both sides).
inline void applySmoothedGain(float* buf, size_t frames, SmoothedParam& g, float coeff) {
    if (g.settled()) {
        const float v = g.current();
        if (v == 1.0f) return;
        for (size_t i = 0; i < frames * 2; ++i) buf[i] *= v;
        return;
    }
    for (size_t i = 0; i < frames; ++i) {
        const float v = g.next(coeff);
        buf[i * 2] *= v;
        buf[i * 2 + 1] *= v;
    }
}

}  // namespace dbdeck
