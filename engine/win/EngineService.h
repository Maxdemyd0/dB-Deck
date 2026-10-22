#pragma once
// The engine's control thread. Owns the configuration (authoritative state),
// Windows integration (devices, sessions, endpoint volume), capture sources,
// playback streams and the mixer, and serves IPC commands.
//
// Threading: every member is touched only by the control thread. Other
// threads (IPC clients, COM notification callbacks) communicate by posting
// closures through post().

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "AppOutputRouting.h"
#include "AudioEngine.h"
#include "DeviceManager.h"
#include "EndpointVolumeController.h"
#include "PipeServer.h"
#include "SessionManager.h"
#include "WasapiStreams.h"
#include "audio/ToneGenerator.h"
#include "config/ConfigStore.h"
#include "graph/GraphCompiler.h"
#include "graph/Mixer.h"
#include "graph/Validation.h"
#include "protocol/Protocol.h"

namespace dbdeck::win {

struct EngineOptions {
    std::filesystem::path configDir;
    std::wstring pipeName;
    bool enableIpc = true;
};

class EngineService {
public:
    explicit EngineService(EngineOptions opts);
    ~EngineService();

    bool start(std::string& error);  // control thread (the caller's thread)
    void run();                      // blocks until shutdown
    void requestShutdown();
    void post(std::function<void()> task);  // any thread

    // Direct command execution on the control thread (used by run() and tests).
    json execute(const std::string& cmd, const json& args);
    json buildState();

private:
    using Handler = std::function<json(const json&)>;

    struct ChannelSource {
        std::vector<std::shared_ptr<CaptureStream>> captures;
        std::shared_ptr<ToneGenerator> tone;
        std::string status;  // see channelStatus()
        std::string error;
        std::set<uint32_t> pids;
    };

    void registerCommands();
    void handleMessage(uint64_t client, const std::string& line);
    void tick();

    ProfileConfig& profile();
    json mutate(bool undoable, bool structural, const std::function<json(ProfileConfig&)>& fn);
    void rebuildAudio();
    void reconcileSources();
    void reconcileOutputs();
    void compile();
    void applySessionVolumes();
    // Pushes continuous parameters (gains, sends, effects, session volumes) without a rebuild.
    void applyLiveParams();
    void adoptExternalSessionChanges();
    void refreshDevices();
    void adoptDevicesByName();
    void refreshSessions();
    void rebindEndpointVolumes();
    void markDirty();
    void markStateDirty() { stateDirty_ = true; }
    void broadcastState();
    void broadcastMeters();
    void broadcastWindowsVolume();

    std::string resolveBusDevice(const BusConfig& b, bool* usingFallback = nullptr) const;
    const DeviceInfo* findDevice(const std::string& id) const;
    std::set<std::string> sessionDevicesFor(const ChannelConfig& c) const;
    std::string sendBlockedReason(const ChannelConfig& c, const SendConfig& s, const BusConfig& b) const;
    std::string channelStatus(const ChannelConfig& c) const;
    json channelRuntimeJson(const ChannelConfig& c);
    json busRuntimeJson(const BusConfig& b);
    json availableAppsJson();
    json windowsVolumeJson() const;
    json diagnosticsJson();
    json driverStateJson() const;
    CablePairs cablePairs() const;

    EngineOptions opts_;
    ConfigStore store_;
    AppConfig config_;
    std::vector<std::string> warnings_;

    ComApartment com_;
    DeviceManager devices_;
    std::vector<DeviceInfo> deviceList_;
    std::unique_ptr<SessionManager> sessions_;
    Mixer mixer_;
    RuntimeRegistry registry_;
    std::unique_ptr<AudioEngine> audio_;
    std::map<std::string, ChannelSource> sources_;
    std::unique_ptr<EndpointVolumeController> defaultVolume_;
    std::unique_ptr<EndpointVolumeController> mainVolume_;
    std::unique_ptr<PipeServer> pipe_;
    std::unique_ptr<AppOutputRouting> appOutput_;
    std::set<uint64_t> helloClients_;

    std::unordered_map<std::string, Handler> handlers_;
    std::deque<json> undo_;

    std::mutex taskMutex_;
    std::deque<std::function<void()>> tasks_;
    UniqueHandle taskEvent_;
    bool quit_ = false;

    using Clock = std::chrono::steady_clock;
    Clock::time_point started_ = Clock::now();
    Clock::time_point lastMeters_, lastHealth_, lastSessions_, lastOutputsRetry_, lastStateSent_, lastVolumeRefresh_;
    Clock::time_point dirtySince_;
    bool dirty_ = false;
    bool stateDirty_ = true;
    bool sessionsDirty_ = true;
    bool devicesDirty_ = true;
    std::map<std::string, std::pair<float, Clock::time_point>> lastSessionApply_;  // channel -> (volume, when)
    uint64_t iconsVersion_ = 0;
    std::set<std::string> knownIconPaths_;
};

}  // namespace dbdeck::win
