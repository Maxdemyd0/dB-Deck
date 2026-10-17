#pragma once
// Device sample-format description and conversion to/from the engine's
// internal format: interleaved stereo 32-bit float.

#include <cstddef>
#include <cstdint>
#include <string>

namespace dbdeck {

constexpr double kEngineSampleRate = 48000.0;
constexpr size_t kMaxBlockFrames = 4096;

enum class SampleType : uint8_t { Float32, Int16, Int24, Int32, Unknown };

struct DeviceFormat {
    uint32_t sampleRate = 48000;
    uint16_t channels = 2;
    SampleType type = SampleType::Float32;
    uint16_t containerBytes = 4;  // bytes per sample container (Int24 may sit in 3 or 4)
    uint16_t validBits = 32;
    uint32_t channelMask = 0;     // WAVEFORMATEXTENSIBLE speaker mask, 0 = unknown

    uint32_t blockAlign() const { return static_cast<uint32_t>(channels) * containerBytes; }
    bool supported() const { return type != SampleType::Unknown && channels > 0; }
    std::string describe() const;
};

// Converts `frames` device frames into interleaved stereo float.
// Mono is duplicated to both sides (dual-mono). Multichannel input is folded
// down using the speaker mask: front L/R pass through, center and surrounds
// are mixed in at -3 dB, LFE is dropped.
void deviceToStereo(const void* src, size_t frames, const DeviceFormat& fmt, float* dst);

// Converts interleaved stereo float into `frames` device frames. Stereo goes
// to the first two channels (front L/R); remaining channels are silent. For a
// mono device, L and R are averaged. Integer output is clamped.
void stereoToDevice(const float* src, size_t frames, const DeviceFormat& fmt, void* dst);

}  // namespace dbdeck
