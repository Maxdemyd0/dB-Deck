#pragma once
// Real-time effect processors applied to managed PCM in the engine.
// Parameters are atomics written by the control thread; coefficient updates
// and all state live on the audio thread. Enable/bypass crossfades over a few
// milliseconds so toggling an effect never clicks.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "audio/Format.h"
#include "audio/Gain.h"

namespace dbdeck {

enum class EffectType : uint8_t { Gain, Eq, Gate, Compressor, Limiter };

struct ParamSpec {
    const char* name;
    float min;
    float max;
    float def;
    const char* unit;
};

const std::vector<ParamSpec>& effectParamSpecs(EffectType type);
std::optional<EffectType> effectTypeFromString(std::string_view s);
const char* effectTypeName(EffectType type);
const std::vector<EffectType>& allEffectTypes();

class Effect {
public:
    static constexpr size_t kMaxParams = 12;

    Effect(EffectType type, double rate);
    virtual ~Effect() = default;
    Effect(const Effect&) = delete;
    Effect& operator=(const Effect&) = delete;

    EffectType type() const { return type_; }

    // Control thread. Values are clamped to the parameter range.
    bool setParam(std::string_view name, float value);
    float param(std::string_view name) const;
    void setEnabled(bool on) { wet_.setTarget(on ? 1.0f : 0.0f); }
    bool enabled() const { return wet_.target() > 0.5f; }
    float gainReductionDb() const { return grDb_.load(std::memory_order_relaxed); }

    // Audio thread.
    void process(float* stereo, size_t frames);

protected:
    float p(size_t i) const { return params_[i].load(std::memory_order_relaxed); }
    virtual void update() = 0;
    virtual void processImpl(float* stereo, size_t frames) = 0;
    virtual void reset() {}

    const double rate_;
    std::atomic<float> grDb_{0.0f};

private:
    const EffectType type_;
    const std::vector<ParamSpec>& specs_;
    std::array<std::atomic<float>, kMaxParams> params_;
    std::atomic<uint32_t> version_{1};
    uint32_t seen_ = 0;
    SmoothedParam wet_{1.0f};
    float wetCoeff_;
    std::vector<float> dry_;
};

std::unique_ptr<Effect> createEffect(EffectType type, double rate = kEngineSampleRate);

// Direct-form-II-transposed biquad, stereo.
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1[2] = {0, 0}, z2[2] = {0, 0};

    void setPeaking(double rate, double freq, double q, double gainDb);
    void setLowShelf(double rate, double freq, double gainDb);
    void setHighShelf(double rate, double freq, double gainDb);
    void process(float* stereo, size_t frames);
    void clear() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }
};

}  // namespace dbdeck
