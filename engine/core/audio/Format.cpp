#include "audio/Format.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace dbdeck {

namespace {

constexpr uint32_t kFL = 0x1, kFR = 0x2, kFC = 0x4, kLFE = 0x8, kBL = 0x10, kBR = 0x20,
                   kFLC = 0x40, kFRC = 0x80, kBC = 0x100, kSL = 0x200, kSR = 0x400;
constexpr float kMinus3dB = 0.70710678f;

uint32_t defaultMask(uint16_t channels) {
    switch (channels) {
        case 1: return kFC;
        case 2: return kFL | kFR;
        case 3: return kFL | kFR | kFC;
        case 4: return kFL | kFR | kBL | kBR;
        case 5: return kFL | kFR | kFC | kBL | kBR;
        case 6: return kFL | kFR | kFC | kLFE | kBL | kBR;
        case 7: return kFL | kFR | kFC | kLFE | kBC | kSL | kSR;
        case 8: return kFL | kFR | kFC | kLFE | kBL | kBR | kSL | kSR;
        default: return 0;
    }
}

// Left/right fold-down weights for each input channel.
struct Fold {
    std::array<float, 32> l{};
    std::array<float, 32> r{};
};

Fold makeFold(const DeviceFormat& fmt) {
    Fold f;
    uint32_t mask = fmt.channelMask ? fmt.channelMask : defaultMask(fmt.channels);
    int ch = 0;
    for (uint32_t bit = 0; bit < 32 && ch < fmt.channels && ch < 32; ++bit) {
        const uint32_t pos = 1u << bit;
        if (!(mask & pos)) continue;
        switch (pos) {
            case kFL: case kFLC: f.l[ch] = 1.0f; break;
            case kFR: case kFRC: f.r[ch] = 1.0f; break;
            case kFC: case kBC: f.l[ch] = f.r[ch] = kMinus3dB; break;
            case kBL: case kSL: f.l[ch] = kMinus3dB; break;
            case kBR: case kSR: f.r[ch] = kMinus3dB; break;
            case kLFE: break;
            default: break;
        }
        ++ch;
    }
    // Channels not described by the mask: treat the first two as L/R.
    if (mask == 0) {
        if (fmt.channels >= 1) f.l[0] = 1.0f;
        if (fmt.channels >= 2) f.r[1] = 1.0f;
    }
    return f;
}

inline float readSample(const uint8_t* p, const DeviceFormat& fmt) {
    switch (fmt.type) {
        case SampleType::Float32: {
            float v;
            std::memcpy(&v, p, 4);
            return v;
        }
        case SampleType::Int16: {
            int16_t v;
            std::memcpy(&v, p, 2);
            return static_cast<float>(v) * (1.0f / 32768.0f);
        }
        case SampleType::Int24: {
            if (fmt.containerBytes == 3) {
                int32_t v = (static_cast<int32_t>(p[0]) << 8) | (static_cast<int32_t>(p[1]) << 16) |
                            (static_cast<int32_t>(p[2]) << 24);
                return static_cast<float>(v) * (1.0f / 2147483648.0f);
            }
            int32_t v;  // 24 valid bits left-justified in a 32-bit container
            std::memcpy(&v, p, 4);
            return static_cast<float>(v) * (1.0f / 2147483648.0f);
        }
        case SampleType::Int32: {
            int32_t v;
            std::memcpy(&v, p, 4);
            return static_cast<float>(v) * (1.0f / 2147483648.0f);
        }
        default: return 0.0f;
    }
}

inline void writeSample(uint8_t* p, float v, const DeviceFormat& fmt) {
    switch (fmt.type) {
        case SampleType::Float32: std::memcpy(p, &v, 4); break;
        case SampleType::Int16: {
            const float c = std::clamp(v, -1.0f, 1.0f);
            const int16_t s = static_cast<int16_t>(std::clamp(c * 32768.0f, -32768.0f, 32767.0f));
            std::memcpy(p, &s, 2);
            break;
        }
        case SampleType::Int24:
        case SampleType::Int32: {
            const double c = std::clamp(static_cast<double>(v), -1.0, 1.0);
            int32_t s = static_cast<int32_t>(std::clamp(c * 2147483648.0, -2147483648.0, 2147483647.0));
            if (fmt.type == SampleType::Int24) s &= ~0xFF;
            if (fmt.containerBytes == 3) {
                p[0] = static_cast<uint8_t>((s >> 8) & 0xFF);
                p[1] = static_cast<uint8_t>((s >> 16) & 0xFF);
                p[2] = static_cast<uint8_t>((s >> 24) & 0xFF);
            } else {
                std::memcpy(p, &s, 4);
            }
            break;
        }
        default: std::memset(p, 0, fmt.containerBytes); break;
    }
}

}  // namespace

std::string DeviceFormat::describe() const {
    const char* t = "unknown";
    switch (type) {
        case SampleType::Float32: t = "float32"; break;
        case SampleType::Int16: t = "int16"; break;
        case SampleType::Int24: t = containerBytes == 3 ? "int24" : "int24in32"; break;
        case SampleType::Int32: t = "int32"; break;
        default: break;
    }
    return std::to_string(sampleRate) + " Hz, " + std::to_string(channels) + " ch, " + t;
}

void deviceToStereo(const void* src, size_t frames, const DeviceFormat& fmt, float* dst) {
    const auto* in = static_cast<const uint8_t*>(src);
    const uint32_t stride = fmt.blockAlign();
    const uint16_t cb = fmt.containerBytes;

    if (fmt.channels == 1) {
        for (size_t i = 0; i < frames; ++i) {
            const float v = readSample(in + i * stride, fmt);
            dst[i * 2] = v;
            dst[i * 2 + 1] = v;
        }
        return;
    }
    if (fmt.channels == 2 && (fmt.channelMask == 0 || fmt.channelMask == (kFL | kFR))) {
        if (fmt.type == SampleType::Float32) {
            std::memcpy(dst, src, frames * 2 * sizeof(float));
            return;
        }
        for (size_t i = 0; i < frames; ++i) {
            dst[i * 2] = readSample(in + i * stride, fmt);
            dst[i * 2 + 1] = readSample(in + i * stride + cb, fmt);
        }
        return;
    }
    const Fold fold = makeFold(fmt);
    const int n = std::min<int>(fmt.channels, 32);
    for (size_t i = 0; i < frames; ++i) {
        float l = 0.0f, r = 0.0f;
        for (int c = 0; c < n; ++c) {
            const float v = readSample(in + i * stride + c * cb, fmt);
            l += v * fold.l[c];
            r += v * fold.r[c];
        }
        dst[i * 2] = l;
        dst[i * 2 + 1] = r;
    }
}

void stereoToDevice(const float* src, size_t frames, const DeviceFormat& fmt, void* dst) {
    auto* out = static_cast<uint8_t*>(dst);
    const uint32_t stride = fmt.blockAlign();
    const uint16_t cb = fmt.containerBytes;

    if (fmt.channels == 2 && fmt.type == SampleType::Float32) {
        std::memcpy(dst, src, frames * 2 * sizeof(float));
        return;
    }
    if (fmt.channels == 1) {
        for (size_t i = 0; i < frames; ++i)
            writeSample(out + i * stride, 0.5f * (src[i * 2] + src[i * 2 + 1]), fmt);
        return;
    }
    std::memset(dst, 0, frames * stride);
    for (size_t i = 0; i < frames; ++i) {
        writeSample(out + i * stride, src[i * 2], fmt);
        writeSample(out + i * stride + cb, src[i * 2 + 1], fmt);
    }
}

}  // namespace dbdeck
