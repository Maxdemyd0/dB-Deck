#pragma once
// AdaptiveStream bridges two clock domains (e.g. a capture device and the
// mixer clock). The producer writes frames at its own rate; the consumer pulls
// exactly the number of frames it needs. A 4-point Hermite resampler converts
// the nominal sample rate and a PI controller nudges the ratio to hold the
// ring fill near a target, absorbing clock drift between devices.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "audio/PullSource.h"
#include "util/StereoRing.h"

namespace dbdeck {

struct AdaptiveStreamStats {
    uint64_t underruns = 0;
    uint64_t overruns = 0;
    uint64_t resyncs = 0;
    size_t fillFrames = 0;
    size_t targetFrames = 0;
    double ratio = 1.0;  // input frames consumed per output frame
    bool starving = true;
};

class AdaptiveStream final : public PullSource {
public:
    // srcRate: producer sample rate. dstRate: consumer sample rate.
    // targetFrames: desired buffered input frames (latency vs. safety).
    AdaptiveStream(double srcRate, double dstRate, size_t targetFrames);

    // Producer side (one thread).
    void write(const float* stereo, size_t frames);

    // Consumer side (one thread). Always fills `frames` frames.
    void pull(float* out, size_t frames) override;

    AdaptiveStreamStats stats() const;
    double sourceRate() const { return srcRate_; }

private:

    StereoRing ring_;
    const double srcRate_;
    const double dstRate_;
    const double nominal_;
    const size_t target_;

    // Consumer-only state.
    float win_[4][2] = {};
    double mu_ = 0.0;
    double ratio_;
    double fillAvg_;
    double integ_ = 0.0;
    bool starving_ = true;

    // Diagnostics, written by either side, read by the control thread.
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> overruns_{0};
    std::atomic<uint64_t> resyncs_{0};
    std::atomic<double> ratioPub_{1.0};
    std::atomic<bool> starvingPub_{true};
};

}  // namespace dbdeck
