#include "AudioEngine.h"

#include "util/Log.h"

namespace dbdeck::win {

AudioEngine::AudioEngine(DeviceManager& devices, Mixer& mixer) : devices_(devices), mixer_(mixer) {
    startTimerClock();
}

AudioEngine::~AudioEngine() { stopAll(); }

void AudioEngine::setBufferSettings(const BufferSettings& b) { buffers_ = b; }

void AudioEngine::stopAll() {
    stopClock();
    for (auto& [id, s] : sinks_) s.stream->stop();
    sinks_.clear();
    masterId_.clear();
}

void AudioEngine::stopClock() {
    if (timerRunning_) {
        timerStop_ = true;
        if (timer_.joinable()) timer_.join();
        timerRunning_ = false;
    }
    if (!masterId_.empty()) {
        auto it = sinks_.find(masterId_);
        if (it != sinks_.end()) {
            it->second.stream->stop();
            sinks_.erase(it);
        }
        masterId_.clear();
    }
    mixer_.setClockRunning(false);
}

void AudioEngine::startTimerClock() {
    mixer_.setClockRunning(true);
    timerStop_ = false;
    timerRunning_ = true;
    timer_ = std::thread([this] { timerLoop(); });
}

void AudioEngine::timerLoop() {
    ComApartment com;
    HANDLE mmcss = enterRealtimeAudioThread();
    UniqueHandle timer(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS));
    if (!timer) timer.reset(CreateWaitableTimerW(nullptr, FALSE, nullptr));
    LARGE_INTEGER due;
    due.QuadPart = -50000;  // 5 ms
    SetWaitableTimer(timer.get(), &due, 5, nullptr, nullptr, FALSE);
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    uint64_t produced = 0;
    while (!timerStop_) {
        WaitForSingleObject(timer.get(), 50);
        QueryPerformanceCounter(&now);
        const uint64_t due_frames =
            static_cast<uint64_t>((now.QuadPart - start.QuadPart) * kEngineSampleRate / freq.QuadPart);
        uint64_t n = due_frames - produced;
        if (n > 9600) {  // large gap (e.g. system resume): resynchronise instead of bursting
            start = now;
            produced = 0;
            continue;
        }
        if (n == 0) continue;
        mixer_.process(nullptr, static_cast<size_t>(n));
        produced += n;
    }
    CancelWaitableTimer(timer.get());
    leaveRealtimeAudioThread(mmcss);
}

bool AudioEngine::reconcile(const std::set<std::string>& wanted, const std::string& masterIdIn) {
    bool changed = false;
    const std::string masterId = wanted.count(masterIdIn) ? masterIdIn : std::string();

    // Remove sinks no longer wanted, or whose role changes.
    for (auto it = sinks_.begin(); it != sinks_.end();) {
        const bool keep = wanted.count(it->first) && (it->second.master == (it->first == masterId));
        if (!keep) {
            if (it->second.master) {
                // Clock is going away: stop it cleanly before anything else.
                stopClock();
                changed = true;
                it = sinks_.begin();
                continue;
            }
            it->second.stream->stop();
            it = sinks_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }

    // Master: open and make it the clock.
    if (!masterId.empty() && !sinks_.count(masterId)) {
        std::string err;
        auto rs = RenderStream::open(devices_, masterId, buffers_, err);
        if (rs) {
            stopClock();  // stops the timer
            mixer_.setClockRunning(true);
            rs->startMaster(&mixer_);
            sinks_[masterId] = Sink{std::move(rs), nullptr, true};
            masterId_ = masterId;
            openErrors_.erase(masterId);
            changed = true;
            log::info("Main output clock running on " + masterId);
        } else {
            if (openErrors_[masterId] != err) log::warn("Cannot open Main output " + masterId + ": " + err);
            openErrors_[masterId] = err;
        }
    }
    if (masterId_.empty() && !timerRunning_) startTimerClock();

    // Slaves.
    for (auto& id : wanted) {
        if (sinks_.count(id) || id == masterId) continue;
        std::string err;
        auto rs = RenderStream::open(devices_, id, buffers_, err);
        if (!rs) {
            if (openErrors_[id] != err) log::warn("Cannot open output " + id + ": " + err);
            openErrors_[id] = err;
            continue;
        }
        const size_t target = std::max<size_t>(rs->periodFrames() * 2, static_cast<size_t>(buffers_.inputTargetMs() * 48));
        auto ring = std::make_shared<AdaptiveStream>(kEngineSampleRate, kEngineSampleRate, target);
        rs->startSlave(ring);
        sinks_[id] = Sink{std::move(rs), ring, false};
        openErrors_.erase(id);
        changed = true;
    }
    for (auto it = openErrors_.begin(); it != openErrors_.end();)
        it = wanted.count(it->first) ? std::next(it) : openErrors_.erase(it);
    return changed;
}

bool AudioEngine::poll() {
    bool changed = false;
    for (auto it = sinks_.begin(); it != sinks_.end();) {
        const StreamStatus st = it->second.stream->status();
        if (st == StreamStatus::Lost || st == StreamStatus::Failed) {
            log::warn("Output " + it->first + " " + streamStatusName(st) + ": " + it->second.stream->error());
            openErrors_[it->first] = std::string(streamStatusName(st)) + ": " + it->second.stream->error();
            if (it->second.master) {
                stopClock();
                startTimerClock();
                it = sinks_.begin();
            } else {
                it->second.stream->stop();
                it = sinks_.erase(it);
            }
            changed = true;
        } else {
            ++it;
        }
    }
    return changed;
}

std::optional<OutputBinding> AudioEngine::binding(const std::string& deviceId) const {
    auto it = sinks_.find(deviceId);
    if (it == sinks_.end()) return std::nullopt;
    return OutputBinding{deviceId, it->second.ring};
}

bool AudioEngine::running(const std::string& deviceId) const {
    auto it = sinks_.find(deviceId);
    return it != sinks_.end() && it->second.stream->status() == StreamStatus::Running;
}

std::vector<OutputStatus> AudioEngine::outputs() const {
    std::vector<OutputStatus> out;
    for (auto& [id, s] : sinks_) {
        OutputStatus o;
        o.deviceId = id;
        o.status = streamStatusName(s.stream->status());
        o.error = s.stream->error();
        o.master = s.master;
        o.latencyMs = s.stream->latencyMs();
        o.bufferFrames = s.stream->bufferFrames();
        o.periodFrames = s.stream->periodFrames();
        o.format = s.stream->format().describe();
        o.glitches = s.stream->glitches();
        if (s.ring) {
            o.ring = s.ring->stats();
            o.hasRing = true;
        }
        out.push_back(std::move(o));
    }
    for (auto& [id, err] : openErrors_) {
        if (sinks_.count(id)) continue;
        OutputStatus o;
        o.deviceId = id;
        o.status = "unavailable";
        o.error = err;
        out.push_back(std::move(o));
    }
    return out;
}

std::string AudioEngine::clockName() const { return masterId_.empty() ? "timer" : "main-output"; }

}  // namespace dbdeck::win
