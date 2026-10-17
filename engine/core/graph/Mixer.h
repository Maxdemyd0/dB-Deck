#pragma once
// Real-time mixing graph.
//
// The control thread compiles the declarative model into an immutable
// CompiledGraph (flat arrays of raw pointers plus preallocated scratch
// buffers) and publishes it with an atomic pointer swap. The clock thread
// (the Main output's render thread, or a timer thread when Main is offline)
// picks the new graph up at the start of the next block and acknowledges its
// epoch; the control thread frees superseded graphs only after that ack. The
// audio thread therefore never allocates, frees, locks or waits.
//
// Continuous parameters (fader gains, send levels, balance, effect params)
// are atomics inside long-lived runtime objects shared by successive graphs,
// so moving a fader never rebuilds anything and smoothing state survives
// graph swaps.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "audio/AdaptiveStream.h"
#include "audio/Format.h"
#include "audio/Gain.h"
#include "audio/Meter.h"
#include "audio/PullSource.h"
#include "dsp/Effects.h"

namespace dbdeck {

struct SendRuntime {
    SmoothedParam gain{0.0f};  // 0 when disabled
};

struct ChannelRuntime {
    SmoothedParam gain{0.0f};     // fader * trim * group, 0 when muted
    SmoothedParam balance{0.0f};
    Meter meter;                  // post-fader
};

struct BusRuntime {
    SmoothedParam gain{0.0f};     // bus fader, 0 when muted
    Meter meter;                  // post-fader, before master mute
};

struct CompiledChannel {
    ChannelRuntime* rt = nullptr;
    std::vector<PullSource*> inputs;
    std::vector<Effect*> effects;
    struct Send {
        uint32_t bus;
        SendRuntime* rt;
        bool preFader;
    };
    std::vector<Send> sends;
};

struct CompiledBus {
    BusRuntime* rt = nullptr;
    std::vector<Effect*> effects;
    struct Input {
        uint32_t fromBus;  // index into CompiledGraph::buses, always earlier in order
        SendRuntime* rt;
        bool preFader;
    };
    std::vector<Input> inputs;
};

struct OutputTarget {
    std::string deviceId;
    AdaptiveStream* ring = nullptr;  // nullptr: written directly by the clock (Main device)
    std::vector<uint32_t> buses;
};

struct CompiledGraph {
    uint64_t epoch = 0;
    std::vector<CompiledChannel> channels;
    std::vector<CompiledBus> buses;  // topological order
    std::vector<OutputTarget> outputs;
    std::vector<std::shared_ptr<void>> keepAlive;  // owns everything the raw pointers reference

    // Allocates scratch buffers. Must be called before publishing.
    void allocate();

    std::vector<float> chanBuf, preBuf, tmpBuf, outBuf, ramp;
    std::vector<std::vector<float>> busBuf, busPreBuf;
};

struct MixerStats {
    uint64_t blocks = 0;
    uint64_t frames = 0;
    double lastBlockUs = 0;
    double maxBlockUs = 0;
};

class Mixer {
public:
    explicit Mixer(double rate = kEngineSampleRate);
    ~Mixer();

    // Control thread.
    void publish(std::unique_ptr<CompiledGraph> graph);
    void collectGarbage();
    // Must be toggled only while no clock thread is calling process().
    void setClockRunning(bool running);
    bool clockRunning() const { return clockRunning_; }
    void setMasterMute(bool mute) { master_.setTarget(mute ? 0.0f : 1.0f); }
    bool masterMuted() const { return master_.target() < 0.5f; }
    MixerStats stats() const;
    uint64_t nextEpoch() { return ++epochCounter_; }

    // Clock thread. Renders `frames` frames. Output for the clock's own device
    // (OutputTarget with ring == nullptr) is written to `directOut` as stereo
    // float; pass nullptr when the clock is a timer. Other outputs are pushed
    // into their rings.
    void process(float* directOut, size_t frames);

private:
    void processChunk(CompiledGraph& g, float* directOut, size_t frames);

    const double rate_;
    const float coeff_;
    std::atomic<CompiledGraph*> pending_{nullptr};
    CompiledGraph* current_ = nullptr;
    std::atomic<uint64_t> ackEpoch_{0};
    std::vector<std::unique_ptr<CompiledGraph>> live_;  // control thread
    bool clockRunning_ = false;
    uint64_t epochCounter_ = 0;
    SmoothedParam master_{1.0f};

    std::atomic<uint64_t> statBlocks_{0}, statFrames_{0};
    std::atomic<double> statLastUs_{0}, statMaxUs_{0};
};

}  // namespace dbdeck
