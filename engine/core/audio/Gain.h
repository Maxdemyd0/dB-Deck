#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>

namespace dbdeck {

inline float dbToLinear(float db) { return db <= -144.0f ? 0.0f : std::pow(10.0f, db / 20.0f); }
inline float linearToDb(float lin) { return lin <= 1e-8f ? -160.0f : 20.0f * std::log10(lin); }

// Fader taper used for managed (engine-processed) audio. Positions 0..1 use a
// squared taper (1.0 = unity / 0 dB, 0.5 = about -12 dB, 0 = silence).
// Positions above 1.0 boost linearly in dB up to kFaderMaxBoostDb at
// kFaderMax, so 0 dB sits at two thirds of the fader travel. Windows session
// volume (application channels) uses the raw 0..1 scalar instead, matching
// the Windows Volume Mixer, and cannot boost.
constexpr float kFaderMax = 1.5f;
constexpr float kFaderMaxBoostDb = 20.0f;

inline float faderToGain(float position) {
    const float p = std::clamp(position, 0.0f, kFaderMax);
    if (p <= 1.0f) return p * p;
    return std::pow(10.0f, (p - 1.0f) / (kFaderMax - 1.0f) * kFaderMaxBoostDb / 20.0f);
}

// Inverse of faderToGain for a level in dB (clamped to the fader range).
inline float dbToFader(float db) {
    if (db <= 0.0f) return std::sqrt(std::pow(10.0f, std::max(db, -144.0f) / 20.0f));
    return 1.0f + std::min(db, kFaderMaxBoostDb) / kFaderMaxBoostDb * (kFaderMax - 1.0f);
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
