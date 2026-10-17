#pragma once
// Signal measurement helpers for audio verification tests.

#include <algorithm>
#include <cmath>
#include <vector>

namespace testsig {

inline double rms(const std::vector<float>& stereo, int channel) {
    double s = 0;
    size_t n = stereo.size() / 2;
    for (size_t i = 0; i < n; ++i) s += static_cast<double>(stereo[i * 2 + channel]) * stereo[i * 2 + channel];
    return n ? std::sqrt(s / n) : 0.0;
}

inline float peak(const std::vector<float>& buf) {
    float p = 0;
    for (float v : buf) p = std::max(p, std::fabs(v));
    return p;
}

// Frequency estimate from interpolated positive-going zero crossings.
inline double estimateFrequency(const std::vector<float>& stereo, int channel, double rate) {
    const size_t n = stereo.size() / 2;
    double first = -1, last = -1;
    int crossings = 0;
    for (size_t i = 1; i < n; ++i) {
        const float a = stereo[(i - 1) * 2 + channel], b = stereo[i * 2 + channel];
        if (a < 0 && b >= 0) {
            const double t = (i - 1) + (-a) / (b - a);
            if (first < 0) first = t;
            last = t;
            ++crossings;
        }
    }
    if (crossings < 2) return 0;
    return (crossings - 1) * rate / (last - first);
}

// Goertzel magnitude of `hz` normalised to sine amplitude.
inline double toneAmplitude(const std::vector<float>& stereo, int channel, double rate, double hz) {
    const size_t n = stereo.size() / 2;
    const double w = 2.0 * 3.14159265358979323846 * hz / rate;
    const double c = 2.0 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; ++i) {
        const double s0 = stereo[i * 2 + channel] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - c * s1 * s2;
    return n ? 2.0 * std::sqrt(std::max(power, 0.0)) / n : 0.0;
}

}  // namespace testsig
