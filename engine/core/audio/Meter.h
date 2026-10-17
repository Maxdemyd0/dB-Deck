#pragma once
// Peak/RMS meter written by the audio thread and drained by the control
// thread. Peaks accumulate (atomic max) until read; RMS is the latest block.

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace dbdeck {

struct MeterReading {
    float peakL = 0, peakR = 0, rmsL = 0, rmsR = 0;
    uint32_t clips = 0;  // blocks containing a sample above full scale since last read
};

class Meter {
public:
    // Audio thread.
    void process(const float* stereo, size_t frames) {
        float pl = 0, pr = 0;
        double sl = 0, sr = 0;
        for (size_t i = 0; i < frames; ++i) {
            const float l = stereo[i * 2], r = stereo[i * 2 + 1];
            const float al = std::fabs(l), ar = std::fabs(r);
            if (al > pl) pl = al;
            if (ar > pr) pr = ar;
            sl += static_cast<double>(l) * l;
            sr += static_cast<double>(r) * r;
        }
        atomicMax(peakL_, pl);
        atomicMax(peakR_, pr);
        if (frames) {
            rmsL_.store(static_cast<float>(std::sqrt(sl / frames)), std::memory_order_relaxed);
            rmsR_.store(static_cast<float>(std::sqrt(sr / frames)), std::memory_order_relaxed);
        }
        if (pl > 1.0f || pr > 1.0f) clips_.fetch_add(1, std::memory_order_relaxed);
        active_.store(true, std::memory_order_relaxed);
    }

    // Externally measured peak (e.g. Windows session meter), mono.
    void setExternalPeak(float peak) {
        atomicMax(peakL_, peak);
        atomicMax(peakR_, peak);
        active_.store(true, std::memory_order_relaxed);
    }

    // Control thread: returns and resets accumulated peaks.
    MeterReading drain() {
        MeterReading m;
        m.peakL = peakL_.exchange(0.0f, std::memory_order_relaxed);
        m.peakR = peakR_.exchange(0.0f, std::memory_order_relaxed);
        m.rmsL = rmsL_.exchange(0.0f, std::memory_order_relaxed);
        m.rmsR = rmsR_.exchange(0.0f, std::memory_order_relaxed);
        m.clips = clips_.exchange(0, std::memory_order_relaxed);
        return m;
    }

    // True if any real measurement was recorded since the last call.
    bool takeActive() { return active_.exchange(false, std::memory_order_relaxed); }

private:
    static void atomicMax(std::atomic<float>& a, float v) {
        float cur = a.load(std::memory_order_relaxed);
        while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
        }
    }

    std::atomic<float> peakL_{0}, peakR_{0}, rmsL_{0}, rmsR_{0};
    std::atomic<uint32_t> clips_{0};
    std::atomic<bool> active_{false};
};

}  // namespace dbdeck
