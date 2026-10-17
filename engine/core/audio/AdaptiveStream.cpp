#include "audio/AdaptiveStream.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dbdeck {

namespace {
// Controller gains. A 100 ppm clock mismatch (typical for consumer devices)
// needs adj = 1e-4, reached with a small steady fill error plus the integrator.
constexpr double kProportional = 0.002;
constexpr double kIntegral = 0.0008;     // per second
constexpr double kMaxAdjust = 0.005;     // +/-0.5% (about 8.6 cents) worst case
constexpr double kMaxIntegral = 0.003;

inline float hermite(float x0, float x1, float x2, float x3, float t) {
    const float c0 = x1;
    const float c1 = 0.5f * (x2 - x0);
    const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
    const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    return ((c3 * t + c2) * t + c1) * t + c0;
}
}  // namespace

AdaptiveStream::AdaptiveStream(double srcRate, double dstRate, size_t targetFrames)
    : ring_(targetFrames * 8 + 8192),
      srcRate_(srcRate),
      dstRate_(dstRate),
      nominal_(srcRate / dstRate),
      target_(std::max<size_t>(targetFrames, 16)),
      ratio_(srcRate / dstRate),
      fillAvg_(static_cast<double>(std::max<size_t>(targetFrames, 16))) {}

void AdaptiveStream::write(const float* stereo, size_t frames) {
    const size_t written = ring_.push(stereo, frames);
    if (written < frames) overruns_.fetch_add(1, std::memory_order_relaxed);
}

void AdaptiveStream::pull(float* out, size_t frames) {
    const size_t avail = ring_.readable();

    if (starving_) {
        if (avail < target_) {
            std::memset(out, 0, frames * 2 * sizeof(float));
            return;
        }
        starving_ = false;
        starvingPub_.store(false, std::memory_order_relaxed);
        fillAvg_ = static_cast<double>(avail);
        integ_ = 0.0;
        mu_ = 0.0;
        std::memset(win_, 0, sizeof(win_));
    }

    // Hard resync when grossly overfilled (e.g. after a consumer stall):
    // drop the excess instead of slowly playing it out at a shifted pitch.
    size_t base = 0;
    size_t usable = avail;
    if (avail > target_ * 4 + 2048) {
        base = avail - target_;
        usable = target_;
        resyncs_.fetch_add(1, std::memory_order_relaxed);
        fillAvg_ = static_cast<double>(target_);
        integ_ = 0.0;
    }

    // Ratio control, updated once per pull.
    const double dt = static_cast<double>(frames) / dstRate_;
    const double alpha = std::min(1.0, dt / 0.5);
    fillAvg_ += (static_cast<double>(usable) - fillAvg_) * alpha;
    const double err = (fillAvg_ - static_cast<double>(target_)) / static_cast<double>(target_);
    integ_ = std::clamp(integ_ + err * dt * kIntegral, -kMaxIntegral, kMaxIntegral);
    const double adj = std::clamp(kProportional * err + integ_, -kMaxAdjust, kMaxAdjust);
    ratio_ = nominal_ * (1.0 + adj);
    ratioPub_.store(ratio_, std::memory_order_relaxed);

    size_t used = 0;
    for (size_t i = 0; i < frames; ++i) {
        const float t = static_cast<float>(mu_);
        out[i * 2] = hermite(win_[0][0], win_[1][0], win_[2][0], win_[3][0], t);
        out[i * 2 + 1] = hermite(win_[0][1], win_[1][1], win_[2][1], win_[3][1], t);
        mu_ += ratio_;
        while (mu_ >= 1.0) {
            if (used >= usable) {
                // Starved: emit silence for the rest and re-prime before resuming.
                std::memset(out + (i + 1) * 2, 0, (frames - i - 1) * 2 * sizeof(float));
                ring_.consume(base + used);
                underruns_.fetch_add(1, std::memory_order_relaxed);
                starving_ = true;
                starvingPub_.store(true, std::memory_order_relaxed);
                return;
            }
            const float* f = ring_.frameAt(base + used);
            ++used;
            std::memmove(win_[0], win_[1], sizeof(float) * 6);
            win_[3][0] = f[0];
            win_[3][1] = f[1];
            mu_ -= 1.0;
        }
    }
    ring_.consume(base + used);
}

AdaptiveStreamStats AdaptiveStream::stats() const {
    AdaptiveStreamStats s;
    s.underruns = underruns_.load(std::memory_order_relaxed);
    s.overruns = overruns_.load(std::memory_order_relaxed);
    s.resyncs = resyncs_.load(std::memory_order_relaxed);
    s.fillFrames = ring_.readable();
    s.targetFrames = target_;
    s.ratio = ratioPub_.load(std::memory_order_relaxed);
    s.starving = starvingPub_.load(std::memory_order_relaxed);
    return s;
}

}  // namespace dbdeck
