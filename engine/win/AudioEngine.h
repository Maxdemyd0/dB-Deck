#pragma once
// Owns playback streams and the mixer clock.
//
// Clock: the Main output's render thread drives Mixer::process when Main is
// connected (lowest latency, no extra buffering). Otherwise a high-resolution
// timer thread drives it so other buses, meters and inputs keep working.
// Every other playback device is a "slave" fed through a drift-compensated
// AdaptiveStream ring. Only the control thread calls into this class.

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "DeviceManager.h"
#include "WasapiStreams.h"
#include "graph/GraphCompiler.h"
#include "graph/Mixer.h"

namespace dbdeck::win {

struct OutputStatus {
    std::string deviceId;
    std::string status;  // running | lost | failed | opening-failed
    std::string error;
    bool master = false;
    double latencyMs = 0;
    uint32_t bufferFrames = 0;
    uint32_t periodFrames = 0;
    std::string format;
    uint64_t glitches = 0;
    AdaptiveStreamStats ring;
    bool hasRing = false;
};

class AudioEngine {
public:
    AudioEngine(DeviceManager& devices, Mixer& mixer);
    ~AudioEngine();

    void setBufferSettings(const BufferSettings& b);
    const BufferSettings& bufferSettings() const { return buffers_; }

    // Opens/closes playback streams so exactly `wanted` devices play, with
    // `masterId` (may be empty) as the clock. Returns true if the set of live
    // bindings changed (caller must recompile the graph).
    bool reconcile(const std::set<std::string>& wanted, const std::string& masterId);

    // Detects lost/failed streams. Returns true if bindings changed.
    bool poll();

    std::optional<OutputBinding> binding(const std::string& deviceId) const;
    bool running(const std::string& deviceId) const;
    std::vector<OutputStatus> outputs() const;
    std::string clockName() const;
    void stopAll();

private:
    struct Sink {
        std::unique_ptr<RenderStream> stream;
        std::shared_ptr<AdaptiveStream> ring;  // null for master
        bool master = false;
    };
    void stopClock();
    void startTimerClock();
    void timerLoop();

    DeviceManager& devices_;
    Mixer& mixer_;
    BufferSettings buffers_;
    std::map<std::string, Sink> sinks_;
    std::map<std::string, std::string> openErrors_;
    std::string masterId_;  // device acting as clock ("" = timer)
    std::thread timer_;
    std::atomic<bool> timerStop_{false};
    bool timerRunning_ = false;
};

}  // namespace dbdeck::win
