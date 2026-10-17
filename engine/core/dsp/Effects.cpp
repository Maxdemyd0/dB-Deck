#include "dsp/Effects.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace dbdeck {

namespace {

const std::vector<ParamSpec> kGainSpecs = {
    {"gainDb", -60.0f, 24.0f, 0.0f, "dB"},
};
const std::vector<ParamSpec> kEqSpecs = {
    {"lowFreq", 20.0f, 1000.0f, 120.0f, "Hz"},   {"lowGainDb", -18.0f, 18.0f, 0.0f, "dB"},
    {"midFreq", 100.0f, 8000.0f, 1000.0f, "Hz"}, {"midGainDb", -18.0f, 18.0f, 0.0f, "dB"},
    {"midQ", 0.1f, 10.0f, 0.9f, ""},             {"highFreq", 1000.0f, 20000.0f, 8000.0f, "Hz"},
    {"highGainDb", -18.0f, 18.0f, 0.0f, "dB"},
};
const std::vector<ParamSpec> kGateSpecs = {
    {"thresholdDb", -90.0f, 0.0f, -50.0f, "dB"}, {"rangeDb", -90.0f, 0.0f, -80.0f, "dB"},
    {"attackMs", 0.1f, 100.0f, 2.0f, "ms"},      {"holdMs", 0.0f, 1000.0f, 80.0f, "ms"},
    {"releaseMs", 5.0f, 2000.0f, 150.0f, "ms"},
};
const std::vector<ParamSpec> kCompSpecs = {
    {"thresholdDb", -60.0f, 0.0f, -18.0f, "dB"}, {"ratio", 1.0f, 20.0f, 3.0f, ":1"},
    {"attackMs", 0.1f, 200.0f, 10.0f, "ms"},     {"releaseMs", 5.0f, 2000.0f, 120.0f, "ms"},
    {"kneeDb", 0.0f, 24.0f, 6.0f, "dB"},         {"makeupDb", 0.0f, 24.0f, 0.0f, "dB"},
};
const std::vector<ParamSpec> kLimiterSpecs = {
    {"ceilingDb", -24.0f, 0.0f, -1.0f, "dB"},
    {"releaseMs", 1.0f, 1000.0f, 50.0f, "ms"},
};

inline float timeCoeff(float ms, double rate) { return smoothingCoeff(std::max(ms, 0.01f), rate); }

// ---------------------------------------------------------------------------

class GainEffect final : public Effect {
public:
    explicit GainEffect(double rate) : Effect(EffectType::Gain, rate) {}

private:
    void update() override { gain_.setTarget(dbToLinear(p(0))); }
    void processImpl(float* buf, size_t n) override { applySmoothedGain(buf, n, gain_, coeff_); }
    void reset() override { gain_.snap(); }

    SmoothedParam gain_{1.0f};
    float coeff_ = smoothingCoeff(5.0f, rate_);
};

class EqEffect final : public Effect {
public:
    explicit EqEffect(double rate) : Effect(EffectType::Eq, rate) {}

private:
    void update() override {
        low_.setLowShelf(rate_, p(0), p(1));
        mid_.setPeaking(rate_, p(2), p(4), p(3));
        high_.setHighShelf(rate_, p(5), p(6));
        lowOn_ = std::fabs(p(1)) > 0.01f;
        midOn_ = std::fabs(p(3)) > 0.01f;
        highOn_ = std::fabs(p(6)) > 0.01f;
    }
    void processImpl(float* buf, size_t n) override {
        if (lowOn_) low_.process(buf, n);
        if (midOn_) mid_.process(buf, n);
        if (highOn_) high_.process(buf, n);
    }
    void reset() override {
        low_.clear();
        mid_.clear();
        high_.clear();
    }

    Biquad low_, mid_, high_;
    bool lowOn_ = false, midOn_ = false, highOn_ = false;
};

class GateEffect final : public Effect {
public:
    explicit GateEffect(double rate) : Effect(EffectType::Gate, rate) {}

private:
    void update() override {
        threshold_ = dbToLinear(p(0));
        floor_ = dbToLinear(p(1));
        attack_ = timeCoeff(p(2), rate_);
        holdSamples_ = static_cast<uint32_t>(p(3) * 0.001 * rate_);
        release_ = timeCoeff(p(4), rate_);
    }
    void processImpl(float* buf, size_t n) override {
        const float envAtt = timeCoeff(0.5f, rate_), envRel = timeCoeff(20.0f, rate_);
        for (size_t i = 0; i < n; ++i) {
            const float x = std::max(std::fabs(buf[i * 2]), std::fabs(buf[i * 2 + 1]));
            env_ += (x - env_) * (x > env_ ? envAtt : envRel);
            if (env_ >= threshold_) {
                hold_ = holdSamples_;
                open_ = true;
            } else if (hold_ > 0) {
                --hold_;
            } else {
                open_ = false;
            }
            const float target = open_ ? 1.0f : floor_;
            gain_ += (target - gain_) * (target > gain_ ? attack_ : release_);
            buf[i * 2] *= gain_;
            buf[i * 2 + 1] *= gain_;
        }
        grDb_.store(linearToDb(gain_), std::memory_order_relaxed);
    }
    void reset() override {
        env_ = 0;
        gain_ = 1;
        hold_ = 0;
    }

    float threshold_ = 0, floor_ = 0, attack_ = 0, release_ = 0;
    uint32_t holdSamples_ = 0, hold_ = 0;
    float env_ = 0, gain_ = 1;
    bool open_ = false;
};

class CompressorEffect final : public Effect {
public:
    explicit CompressorEffect(double rate) : Effect(EffectType::Compressor, rate) {}

private:
    void update() override {
        threshold_ = p(0);
        ratio_ = std::max(1.0f, p(1));
        attack_ = timeCoeff(p(2), rate_);
        release_ = timeCoeff(p(3), rate_);
        knee_ = p(4);
        makeup_ = dbToLinear(p(5));
    }
    float computeGrDb(float levelDb) const {
        const float over = levelDb - threshold_;
        const float slope = 1.0f / ratio_ - 1.0f;
        if (knee_ > 0.0f && 2.0f * std::fabs(over) <= knee_) {
            const float k = over + knee_ * 0.5f;
            return slope * k * k / (2.0f * knee_);
        }
        return over > 0.0f ? slope * over : 0.0f;
    }
    void processImpl(float* buf, size_t n) override {
        for (size_t i = 0; i < n; ++i) {
            const float x = std::max(std::fabs(buf[i * 2]), std::fabs(buf[i * 2 + 1]));
            const float target = computeGrDb(linearToDb(x));
            // Gain reduction is negative; "attack" means reduction deepening.
            grState_ += (target - grState_) * (target < grState_ ? attack_ : release_);
            const float g = dbToLinear(grState_) * makeup_;
            buf[i * 2] *= g;
            buf[i * 2 + 1] *= g;
        }
        grDb_.store(grState_, std::memory_order_relaxed);
    }
    void reset() override { grState_ = 0; }

    float threshold_ = -18, ratio_ = 3, attack_ = 0, release_ = 0, knee_ = 6, makeup_ = 1;
    float grState_ = 0;
};

class LimiterEffect final : public Effect {
public:
    explicit LimiterEffect(double rate) : Effect(EffectType::Limiter, rate) {}

private:
    void update() override {
        ceiling_ = dbToLinear(p(0));
        release_ = timeCoeff(p(1), rate_);
    }
    void processImpl(float* buf, size_t n) override {
        float minEnv = 1.0f;
        for (size_t i = 0; i < n; ++i) {
            const float x = std::max(std::fabs(buf[i * 2]), std::fabs(buf[i * 2 + 1]));
            const float need = x > ceiling_ ? ceiling_ / x : 1.0f;
            if (need < env_) env_ = need;  // instant attack: no overshoot
            else env_ += (need - env_) * release_;
            buf[i * 2] = std::clamp(buf[i * 2] * env_, -ceiling_, ceiling_);
            buf[i * 2 + 1] = std::clamp(buf[i * 2 + 1] * env_, -ceiling_, ceiling_);
            minEnv = std::min(minEnv, env_);
        }
        grDb_.store(linearToDb(minEnv), std::memory_order_relaxed);
    }
    void reset() override { env_ = 1.0f; }

    float ceiling_ = 1, release_ = 0, env_ = 1;
};

}  // namespace

// ---------------------------------------------------------------------------

const std::vector<ParamSpec>& effectParamSpecs(EffectType type) {
    switch (type) {
        case EffectType::Gain: return kGainSpecs;
        case EffectType::Eq: return kEqSpecs;
        case EffectType::Gate: return kGateSpecs;
        case EffectType::Compressor: return kCompSpecs;
        case EffectType::Limiter: return kLimiterSpecs;
    }
    return kGainSpecs;
}

const std::vector<EffectType>& allEffectTypes() {
    static const std::vector<EffectType> all = {EffectType::Gain, EffectType::Eq, EffectType::Gate,
                                                EffectType::Compressor, EffectType::Limiter};
    return all;
}

const char* effectTypeName(EffectType type) {
    switch (type) {
        case EffectType::Gain: return "gain";
        case EffectType::Eq: return "eq";
        case EffectType::Gate: return "gate";
        case EffectType::Compressor: return "compressor";
        case EffectType::Limiter: return "limiter";
    }
    return "gain";
}

std::optional<EffectType> effectTypeFromString(std::string_view s) {
    for (auto t : allEffectTypes())
        if (s == effectTypeName(t)) return t;
    return std::nullopt;
}

Effect::Effect(EffectType type, double rate)
    : rate_(rate), type_(type), specs_(effectParamSpecs(type)), wetCoeff_(smoothingCoeff(4.0f, rate)) {
    for (size_t i = 0; i < specs_.size() && i < kMaxParams; ++i)
        params_[i].store(specs_[i].def, std::memory_order_relaxed);
    dry_.assign(kMaxBlockFrames * 2, 0.0f);
}

bool Effect::setParam(std::string_view name, float value) {
    for (size_t i = 0; i < specs_.size(); ++i) {
        if (name == specs_[i].name) {
            if (!std::isfinite(value)) return false;
            const float v = std::clamp(value, specs_[i].min, specs_[i].max);
            if (params_[i].load(std::memory_order_relaxed) == v) return true;
            params_[i].store(v, std::memory_order_relaxed);
            version_.fetch_add(1, std::memory_order_release);
            return true;
        }
    }
    return false;
}

float Effect::param(std::string_view name) const {
    for (size_t i = 0; i < specs_.size(); ++i)
        if (name == specs_[i].name) return params_[i].load(std::memory_order_relaxed);
    return 0.0f;
}

void Effect::process(float* buf, size_t frames) {
    const uint32_t v = version_.load(std::memory_order_acquire);
    if (v != seen_) {
        const bool first = seen_ == 0;
        seen_ = v;
        update();
        if (first) reset();
    }
    if (wet_.settled()) {
        if (wet_.current() <= 0.0f) {
            grDb_.store(0.0f, std::memory_order_relaxed);
            return;  // fully bypassed
        }
        processImpl(buf, frames);
        return;
    }
    // Crossfading between dry and processed signal.
    const size_t n = std::min(frames, kMaxBlockFrames);
    std::memcpy(dry_.data(), buf, n * 2 * sizeof(float));
    processImpl(buf, n);
    for (size_t i = 0; i < n; ++i) {
        const float w = wet_.next(wetCoeff_);
        buf[i * 2] = dry_[i * 2] + (buf[i * 2] - dry_[i * 2]) * w;
        buf[i * 2 + 1] = dry_[i * 2 + 1] + (buf[i * 2 + 1] - dry_[i * 2 + 1]) * w;
    }
}

std::unique_ptr<Effect> createEffect(EffectType type, double rate) {
    switch (type) {
        case EffectType::Gain: return std::make_unique<GainEffect>(rate);
        case EffectType::Eq: return std::make_unique<EqEffect>(rate);
        case EffectType::Gate: return std::make_unique<GateEffect>(rate);
        case EffectType::Compressor: return std::make_unique<CompressorEffect>(rate);
        case EffectType::Limiter: return std::make_unique<LimiterEffect>(rate);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// RBJ Audio EQ Cookbook biquads.

void Biquad::setPeaking(double rate, double freq, double q, double gainDb) {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * std::numbers::pi * std::min(freq, rate * 0.49) / rate;
    const double alpha = std::sin(w0) / (2.0 * std::max(q, 0.05));
    const double c = std::cos(w0);
    const double a0 = 1.0 + alpha / A;
    b0 = static_cast<float>((1.0 + alpha * A) / a0);
    b1 = static_cast<float>((-2.0 * c) / a0);
    b2 = static_cast<float>((1.0 - alpha * A) / a0);
    a1 = static_cast<float>((-2.0 * c) / a0);
    a2 = static_cast<float>((1.0 - alpha / A) / a0);
}

void Biquad::setLowShelf(double rate, double freq, double gainDb) {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * std::numbers::pi * std::min(freq, rate * 0.49) / rate;
    const double c = std::cos(w0), s = std::sin(w0);
    const double alpha = s / 2.0 * std::sqrt(2.0);  // shelf slope S = 1
    const double sa = 2.0 * std::sqrt(A) * alpha;
    const double a0 = (A + 1) + (A - 1) * c + sa;
    b0 = static_cast<float>(A * ((A + 1) - (A - 1) * c + sa) / a0);
    b1 = static_cast<float>(2 * A * ((A - 1) - (A + 1) * c) / a0);
    b2 = static_cast<float>(A * ((A + 1) - (A - 1) * c - sa) / a0);
    a1 = static_cast<float>(-2 * ((A - 1) + (A + 1) * c) / a0);
    a2 = static_cast<float>(((A + 1) + (A - 1) * c - sa) / a0);
}

void Biquad::setHighShelf(double rate, double freq, double gainDb) {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * std::numbers::pi * std::min(freq, rate * 0.49) / rate;
    const double c = std::cos(w0), s = std::sin(w0);
    const double alpha = s / 2.0 * std::sqrt(2.0);
    const double sa = 2.0 * std::sqrt(A) * alpha;
    const double a0 = (A + 1) - (A - 1) * c + sa;
    b0 = static_cast<float>(A * ((A + 1) + (A - 1) * c + sa) / a0);
    b1 = static_cast<float>(-2 * A * ((A - 1) + (A + 1) * c) / a0);
    b2 = static_cast<float>(A * ((A + 1) + (A - 1) * c - sa) / a0);
    a1 = static_cast<float>(2 * ((A - 1) - (A + 1) * c) / a0);
    a2 = static_cast<float>(((A + 1) - (A - 1) * c - sa) / a0);
}

void Biquad::process(float* buf, size_t frames) {
    for (size_t i = 0; i < frames; ++i) {
        for (int ch = 0; ch < 2; ++ch) {
            const float x = buf[i * 2 + ch];
            const float y = b0 * x + z1[ch];
            z1[ch] = b1 * x - a1 * y + z2[ch];
            z2[ch] = b2 * x - a2 * y;
            buf[i * 2 + ch] = y;
        }
    }
}

}  // namespace dbdeck
