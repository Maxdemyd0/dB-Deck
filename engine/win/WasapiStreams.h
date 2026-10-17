#pragma once
// WASAPI shared-mode streams.
//
// CaptureStream: hardware capture, endpoint loopback, or per-process
// loopback (ActivateAudioInterfaceAsync + PROCESS_LOOPBACK). A dedicated MMCSS
// thread converts packets to stereo float and writes them into an
// AdaptiveStream that the mixer pulls at the engine rate.
//
// RenderStream: event-driven playback. As the clock master it calls
// Mixer::process directly for its device; as a slave it pulls an
// AdaptiveStream filled by the mixer (drift-compensated).

#include "ComUtil.h"
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "DeviceManager.h"
#include "audio/AdaptiveStream.h"
#include "audio/Format.h"
#include "audio/Gain.h"
#include "graph/Mixer.h"

namespace dbdeck::win {

enum class StreamStatus { Running, Lost, Failed, Stopped };
const char* streamStatusName(StreamStatus s);

struct BufferSettings {
    std::string mode = "normal";  // low | normal | safe
    double inputTargetMs() const { return mode == "low" ? 6.0 : mode == "safe" ? 30.0 : 12.0; }
    REFERENCE_TIME renderBuffer() const { return mode == "low" ? 0 : mode == "safe" ? 300000 : 100000; }  // 100ns units
    REFERENCE_TIME captureBuffer() const { return mode == "safe" ? 400000 : 200000; }
    bool lowLatency() const { return mode == "low"; }
};

class CaptureStream {
public:
    enum class Kind { Device, Loopback, Process };

    static std::shared_ptr<CaptureStream> openDevice(DeviceManager& dm, const std::string& deviceId, bool loopback,
                                                     const BufferSettings& buf, std::string& error);
    static std::shared_ptr<CaptureStream> openProcess(uint32_t pid, const BufferSettings& buf, std::string& error);

    ~CaptureStream();
    void stop();

    std::shared_ptr<AdaptiveStream> stream() const { return stream_; }
    StreamStatus status() const { return status_.load(); }
    const std::string& error() const { return error_; }
    const DeviceFormat& format() const { return format_; }
    Kind kind() const { return kind_; }
    uint32_t pid() const { return pid_; }
    const std::string& deviceId() const { return deviceId_; }
    double bufferMs() const { return bufferMs_; }

private:
    CaptureStream() = default;
    bool initClient(ComPtr<IAudioClient> client, const WAVEFORMATEX* fmt, DWORD flags, const BufferSettings& buf,
                    std::string& error);
    void run();

    Kind kind_ = Kind::Device;
    uint32_t pid_ = 0;
    std::string deviceId_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    UniqueHandle event_;
    DeviceFormat format_;
    std::shared_ptr<AdaptiveStream> stream_;
    std::vector<float> scratch_;
    double bufferMs_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<StreamStatus> status_{StreamStatus::Stopped};
    std::string error_;
};

class RenderStream {
public:
    static std::unique_ptr<RenderStream> open(DeviceManager& dm, const std::string& deviceId,
                                              const BufferSettings& buf, std::string& error);
    ~RenderStream();

    // Exactly one of these, once.
    void startMaster(Mixer* mixer);
    void startSlave(std::shared_ptr<AdaptiveStream> ring);
    void stop();

    StreamStatus status() const { return status_.load(); }
    const std::string& error() const { return error_; }
    const std::string& deviceId() const { return deviceId_; }
    const DeviceFormat& format() const { return format_; }
    uint32_t bufferFrames() const { return bufferFrames_; }
    uint32_t periodFrames() const { return periodFrames_; }
    double latencyMs() const;
    bool isMaster() const { return mixer_ != nullptr; }
    uint64_t glitches() const { return glitches_.load(); }

private:
    RenderStream() = default;
    void run();

    std::string deviceId_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    UniqueHandle event_;
    DeviceFormat format_;
    uint32_t bufferFrames_ = 0;
    uint32_t periodFrames_ = 0;
    REFERENCE_TIME streamLatency_ = 0;
    Mixer* mixer_ = nullptr;
    std::shared_ptr<AdaptiveStream> ring_;
    std::vector<float> scratch_;
    SmoothedParam fade_{0.0f};
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<StreamStatus> status_{StreamStatus::Stopped};
    std::atomic<uint64_t> glitches_{0};
    std::string error_;
};

}  // namespace dbdeck::win
