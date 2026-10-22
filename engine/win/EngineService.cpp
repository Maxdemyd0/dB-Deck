#include "EngineService.h"

#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "config/VoicemeeterImport.h"
#include "graph/AppMatch.h"
#include "graph/Scenes.h"
#include "graph/Validation.h"
#include "util/Log.h"

namespace dbdeck::win {

namespace {

constexpr const char* kEngineVersion = "1.0.0";

std::map<uint32_t, uint32_t> parentPids() {
    std::map<uint32_t, uint32_t> m;
    UniqueHandle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap) return m;
    PROCESSENTRY32W pe{sizeof(pe)};
    if (Process32FirstW(snap.get(), &pe)) {
        do {
            m[pe.th32ProcessID] = pe.th32ParentProcessID;
        } while (Process32NextW(snap.get(), &pe));
    }
    return m;
}

// Endpoints created by dB-Deck's own driver (driver/windows/dbdeck-vad).
bool isDbDeckDriverDevice(const DeviceInfo& d) {
    return d.adapterName.rfind("dB-Deck Virtual Cable", 0) == 0;
}

json deviceJson(const DeviceInfo& d) {
    return {{"id", d.id},
            {"name", d.name},
            {"adapterName", d.adapterName},
            {"flow", d.render ? "render" : "capture"},
            {"state", d.state},
            {"isDefault", d.isDefault},
            {"isDefaultComms", d.isDefaultComms},
            {"likelyVirtual", d.likelyVirtual},
            {"dbdeckDriver", isDbDeckDriverDevice(d)},
            {"format", d.formatKnown ? d.mixFormat.describe() : ""}};
}

json streamStatsJson(const AdaptiveStreamStats& s) {
    return {{"underruns", s.underruns}, {"overruns", s.overruns},   {"resyncs", s.resyncs},
            {"fillFrames", s.fillFrames}, {"targetFrames", s.targetFrames}, {"ratio", s.ratio},
            {"starving", s.starving}};
}

json volumeStateJson(const EndpointVolumeState& s) {
    return {{"bound", s.bound},
            {"deviceId", s.deviceId},
            {"deviceName", s.deviceName},
            {"available", s.available},
            {"unavailableReason", s.unavailableReason},
            {"volume", s.volume},
            {"percent", static_cast<int>(s.volume * 100.0f + 0.5f)},
            {"mute", s.mute},
            {"levelDb", s.levelDb},
            {"minDb", s.minDb},
            {"maxDb", s.maxDb},
            {"steps", s.steps},
            {"hardwareVolume", s.hardwareVolume},
            {"lastChangeSource", s.lastChangeSource},
            {"changeCount", s.changeCount}};
}

void requireFound(const void* p, const std::string& what) {
    if (!p) throw ProtocolError("not_found", what + " not found");
}

}  // namespace

// ---------------------------------------------------------------------------

EngineService::EngineService(EngineOptions opts) : opts_(std::move(opts)), store_(opts_.configDir) {
    taskEvent_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
}

EngineService::~EngineService() {
    if (pipe_) pipe_->stop();
    if (audio_) audio_->stopAll();
    for (auto& [id, s] : sources_)
        for (auto& c : s.captures) c->stop();
    sources_.clear();
    defaultVolume_.reset();
    mainVolume_.reset();
    if (sessions_) sessions_->shutdown();
    devices_.shutdown();
}

void EngineService::post(std::function<void()> task) {
    {
        std::lock_guard lock(taskMutex_);
        tasks_.push_back(std::move(task));
    }
    SetEvent(taskEvent_.get());
}

void EngineService::requestShutdown() {
    post([this] { quit_ = true; });
}

ProfileConfig& EngineService::profile() {
    ProfileConfig* p = config_.activeProfile();
    if (!p) {
        config_.profiles.push_back(makeDefaultProfile("Default"));
        config_.activeProfileId = config_.profiles.back().id;
        p = &config_.profiles.back();
    }
    return *p;
}

bool EngineService::start(std::string& error) {
    if (!com_.ok()) {
        error = "COM initialization failed";
        return false;
    }
    ConfigLoadResult loaded = store_.load();
    config_ = std::move(loaded.config);
    warnings_ = loaded.warnings;
    for (auto& w : warnings_) log::warn(w);
    if (loaded.createdDefault) markDirty();

    if (!devices_.init(
            [this](const DeviceEvent& e) {
                post([this, e] {
                    devicesDirty_ = true;
                    if (e.kind == DeviceEventKind::DefaultChanged) log::info("Default device changed: " + e.deviceId);
                });
            },
            error))
        return false;
    appOutput_ = std::make_unique<AppOutputRouting>();
    if (!appOutput_->available()) log::warn(appOutput_->unavailableReason());
    sessions_ = std::make_unique<SessionManager>(devices_, [this] { post([this] { sessionsDirty_ = true; }); });

    auto volumeNotify = [this](bool isMain) {
        return [this, isMain](const std::string& deviceId, float v, bool m, bool ours) {
            post([this, isMain, deviceId, v, m, ours] {
                auto& ctl = isMain ? mainVolume_ : defaultVolume_;
                if (ctl && ctl->deviceId() == deviceId) ctl->onNotification(v, m, ours);
                // The other controller may be bound to the same endpoint.
                auto& other = isMain ? defaultVolume_ : mainVolume_;
                if (other && other->deviceId() == deviceId) other->onNotification(v, m, ours);
                broadcastWindowsVolume();
            });
        };
    };
    defaultVolume_ = std::make_unique<EndpointVolumeController>(volumeNotify(false));
    mainVolume_ = std::make_unique<EndpointVolumeController>(volumeNotify(true));

    audio_ = std::make_unique<AudioEngine>(devices_, mixer_);
    BufferSettings b;
    b.mode = config_.settings.bufferMode;
    audio_->setBufferSettings(b);

    registerCommands();
    refreshDevices();
    refreshSessions();
    rebuildAudio();

    if (opts_.enableIpc) {
        pipe_ = std::make_unique<PipeServer>();
        if (!pipe_->start(
                opts_.pipeName,
                [this](uint64_t client, std::string line) {
                    post([this, client, line = std::move(line)] { handleMessage(client, line); });
                },
                [](uint64_t client) { log::info("IPC client " + std::to_string(client) + " connected"); },
                [this](uint64_t client) {
                    post([this, client] { helloClients_.erase(client); });
                    log::info("IPC client " + std::to_string(client) + " disconnected");
                },
                error))
            return false;
    }
    log::info("dB-Deck engine started");
    return true;
}

void EngineService::run() {
    while (!quit_) {
        const DWORD timeout = static_cast<DWORD>(std::max(5, 1000 / std::max(5, config_.settings.meterRateHz) / 2));
        WaitForSingleObject(taskEvent_.get(), timeout);
        std::deque<std::function<void()>> batch;
        {
            std::lock_guard lock(taskMutex_);
            batch.swap(tasks_);
        }
        for (auto& t : batch) {
            try {
                t();
            } catch (const std::exception& e) {
                log::error(std::string("task failed: ") + e.what());
            }
        }
        try {
            tick();
        } catch (const std::exception& e) {
            log::error(std::string("tick failed: ") + e.what());
        }
    }
    if (dirty_) {
        std::string err;
        if (!store_.save(config_, err)) log::error("save failed: " + err);
    }
    log::info("dB-Deck engine stopping");
}

void EngineService::tick() {
    const auto now = Clock::now();
    auto since = [&](Clock::time_point t) { return std::chrono::duration<double, std::milli>(now - t).count(); };

    if (devicesDirty_) {
        devicesDirty_ = false;
        refreshDevices();
        sessionsDirty_ = true;
        rebuildAudio();
    }
    if (sessionsDirty_ || since(lastSessions_) > 1000) {
        const bool forced = sessionsDirty_;
        sessionsDirty_ = false;
        lastSessions_ = now;
        refreshSessions();
        adoptExternalSessionChanges();
        applySessionVolumes();
        // App captures follow processes appearing/disappearing.
        bool anyCapture = false;
        for (auto& c : profile().channels) anyCapture |= c.type == ChannelType::App && c.appCapture;
        if (anyCapture) {
            std::map<std::string, std::set<uint32_t>> before;
            for (auto& [id, s] : sources_) before[id] = s.pids;
            reconcileSources();
            bool changed = false;
            for (auto& [id, s] : sources_) changed |= before[id] != s.pids;
            if (changed) compile();
        }
        if (forced) markStateDirty();
        else markStateDirty();  // session states/levels may have changed
    }
    if (since(lastHealth_) > 250) {
        lastHealth_ = now;
        bool changed = audio_->poll();
        for (auto& [id, s] : sources_)
            for (auto& c : s.captures)
                if (c->status() == StreamStatus::Lost || c->status() == StreamStatus::Failed) changed = true;
        if (changed) {
            reconcileSources();
            compile();
            rebindEndpointVolumes();
            markStateDirty();
        }
    }
    if (since(lastProcessCheck_) > 3000) {
        lastProcessCheck_ = now;
        auto vm = findVoicemeeter();
        const bool changed = vm.empty() != voicemeeter_.empty();
        voicemeeter_ = std::move(vm);
        if (changed) {
            log::info(voicemeeter_.empty() ? "Voicemeeter closed" : "Voicemeeter is running");
            markStateDirty();
            if (voicemeeter_.empty()) lastOutputsRetry_ = {};  // reclaim its devices now
        }
    }
    if (since(lastOutputsRetry_) > 2000) {
        lastOutputsRetry_ = now;
        // Retry outputs/inputs that should be live (e.g. device returned, format renegotiation).
        bool missing = false;
        for (auto& b : profile().buses) {
            const std::string dev = resolveBusDevice(b);
            if (!dev.empty() && !audio_->running(dev)) missing = true;
        }
        for (auto& c : profile().channels)
            if ((c.type == ChannelType::HardwareInput || c.type == ChannelType::EndpointLoopback) &&
                sources_[c.id].captures.empty() && findDevice(c.deviceId) && findDevice(c.deviceId)->state == "active")
                missing = true;
        if (missing) rebuildAudio();
    }
    if (since(lastVolumeRefresh_) > 2000) {
        lastVolumeRefresh_ = now;
        // Safety net in case a notification was missed.
        const auto a = defaultVolume_->state(), b = mainVolume_->state();
        defaultVolume_->refresh();
        mainVolume_->refresh();
        const auto a2 = defaultVolume_->state(), b2 = mainVolume_->state();
        if (a.volume != a2.volume || a.mute != a2.mute || b.volume != b2.volume || b.mute != b2.mute)
            broadcastWindowsVolume();
    }
    if (since(lastMeters_) >= 1000.0 / std::max(5, config_.settings.meterRateHz)) {
        lastMeters_ = now;
        broadcastMeters();
    }
    if (dirty_ && since(dirtySince_) > 400) {
        std::string err;
        if (store_.save(config_, err)) dirty_ = false;
        else {
            log::error("Saving settings failed: " + err);
            dirtySince_ = now;
        }
    }
    if (stateDirty_ && since(lastStateSent_) > 50) {
        lastStateSent_ = now;
        stateDirty_ = false;
        broadcastState();
    }
}

void EngineService::markDirty() {
    if (!dirty_) dirtySince_ = Clock::now();
    dirty_ = true;
    markStateDirty();
}

// ---------------------------------------------------------------------------
// Windows state

void EngineService::refreshDevices() {
    deviceList_ = devices_.enumerate();
    adoptDevicesByName();
    rebindEndpointVolumes();
    markStateDirty();
}

void EngineService::adoptDevicesByName() {
    // Entries that only know a device by name (e.g. imported while the device
    // was unplugged) bind to its stable ID as soon as a matching endpoint exists.
    auto byName = [&](const std::string& name, bool render) -> const DeviceInfo* {
        if (name.empty()) return nullptr;
        for (auto& d : deviceList_)
            if (d.render == render && d.name == name) return &d;
        return nullptr;
    };
    for (auto& p : config_.profiles) {
        for (auto& b : p.buses)
            if (b.deviceId.empty() && !b.deviceName.empty())
                if (const DeviceInfo* d = byName(b.deviceName, true)) {
                    b.deviceId = d->id;
                    log::info("Bus '" + b.name + "' bound to " + d->name);
                    markDirty();
                }
        for (auto& c : p.channels) {
            if (!c.deviceId.empty() || c.deviceName.empty()) continue;
            if (c.type != ChannelType::HardwareInput && c.type != ChannelType::EndpointLoopback) continue;
            if (const DeviceInfo* d = byName(c.deviceName, c.type == ChannelType::EndpointLoopback)) {
                c.deviceId = d->id;
                log::info("Channel '" + c.name + "' bound to " + d->name);
                markDirty();
            }
        }
    }
}

void EngineService::refreshSessions() {
    sessions_->refresh(deviceList_, GetCurrentProcessId());
    for (auto& s : sessions_->sessions())
        if (!s.exePath.empty() && knownIconPaths_.insert(s.exePath).second) ++iconsVersion_;
}

void EngineService::rebindEndpointVolumes() {
    const std::string def = devices_.defaultDeviceId(true);
    const DeviceInfo* d = findDevice(def);
    if (def.empty()) {
        if (defaultVolume_->state().bound) defaultVolume_->unbind();
    } else if (defaultVolume_->deviceId() != def || !defaultVolume_->state().available) {
        defaultVolume_->bind(devices_.device(def).Get(), def, d ? d->name : def);
        log::info("Windows master volume bound to " + (d ? d->name : def));
    }
    const BusConfig* main = profile().mainBus();
    const std::string mainDev = main ? main->deviceId : std::string();
    const DeviceInfo* md = findDevice(mainDev);
    if (mainDev.empty() || !md || md->state != "active") {
        if (mainVolume_->state().bound) mainVolume_->unbind();
    } else if (mainVolume_->deviceId() != mainDev || !mainVolume_->state().available) {
        mainVolume_->bind(devices_.device(mainDev).Get(), mainDev, md->name);
    }
    broadcastWindowsVolume();
}

const DeviceInfo* EngineService::findDevice(const std::string& id) const {
    if (id.empty()) return nullptr;
    for (auto& d : deviceList_)
        if (d.id == id) return &d;
    return nullptr;
}

std::string EngineService::resolveBusDevice(const BusConfig& b, bool* usingFallback) const {
    if (usingFallback) *usingFallback = false;
    auto ok = [&](const std::string& id) {
        const DeviceInfo* d = findDevice(id);
        return d && d->render && d->state == "active";
    };
    if (ok(b.deviceId)) return b.deviceId;
    // Only explicitly authorised fallbacks; never Windows' default.
    for (auto& f : b.fallbackDeviceIds)
        if (ok(f)) {
            if (usingFallback) *usingFallback = true;
            return f;
        }
    return {};
}

std::set<std::string> EngineService::sessionDevicesFor(const ChannelConfig& c) const {
    std::set<std::string> out;
    for (auto* s : sessions_->matching(c.match)) out.insert(s->deviceId);
    return out;
}

std::string EngineService::sendBlockedReason(const ChannelConfig& c, const SendConfig&, const BusConfig& b) const {
    if (c.type != ChannelType::App || !c.appCapture) return {};
    // Double-audio guard: the app's original audio already plays on its own
    // endpoint. Sending the captured copy to the same endpoint would play it twice.
    std::vector<std::string> reach = reachableBuses(const_cast<EngineService*>(this)->profile(), {b.id});
    const auto devs = sessionDevicesFor(c);
    for (auto& id : reach) {
        const BusConfig* rb = const_cast<EngineService*>(this)->profile().findBus(id);
        if (!rb) continue;
        const std::string dev = resolveBusDevice(*rb);
        if (!dev.empty() && devs.count(dev)) {
            const DeviceInfo* d = findDevice(dev);
            return "Original app audio already plays on " + (d ? d->name : dev) +
                   "; sending the captured copy there would double it";
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Audio graph

void EngineService::rebuildAudio() {
    reconcileSources();
    reconcileOutputs();
    compile();
    applySessionVolumes();
}

void EngineService::reconcileSources() {
    ProfileConfig& p = profile();
    BufferSettings buf = audio_->bufferSettings();
    std::set<std::string> live;
    std::map<uint32_t, uint32_t> parents;
    bool parentsLoaded = false;

    for (auto& c : p.channels) {
        live.insert(c.id);
        ChannelSource& src = sources_[c.id];
        // Drop dead captures.
        std::erase_if(src.captures, [&](const std::shared_ptr<CaptureStream>& cs) {
            if (cs->status() == StreamStatus::Running) return false;
            src.error = std::string(streamStatusName(cs->status())) + ": " + cs->error();
            cs->stop();
            return true;
        });

        switch (c.type) {
            case ChannelType::TestTone: {
                src.captures.clear();
                if (!src.tone) src.tone = std::make_shared<ToneGenerator>(kEngineSampleRate, c.toneHz, 0.25f);
                src.tone->setFrequency(c.toneHz);
                src.status = "generated";
                break;
            }
            case ChannelType::HardwareInput:
            case ChannelType::EndpointLoopback: {
                src.tone.reset();
                const bool loopback = c.type == ChannelType::EndpointLoopback;
                if (!src.captures.empty() && src.captures[0]->deviceId() != c.deviceId) {
                    for (auto& cs : src.captures) cs->stop();
                    src.captures.clear();
                }
                const DeviceInfo* d = findDevice(c.deviceId);
                if (d) c.deviceName = d->name;
                if (!d || d->state != "active") {
                    for (auto& cs : src.captures) cs->stop();
                    src.captures.clear();
                    src.status = "disconnected";
                    break;
                }
                if (src.captures.empty()) {
                    std::string err;
                    auto cs = CaptureStream::openDevice(devices_, c.deviceId, loopback, buf, err);
                    if (cs) {
                        src.captures.push_back(cs);
                        src.error.clear();
                        log::info("Capture started: " + d->name + " (" + cs->format().describe() + ")");
                    } else {
                        if (src.error != err) log::warn("Capture failed for " + d->name + ": " + err);
                        src.error = err;
                    }
                }
                src.status = src.captures.empty() ? "error" : "connected";
                break;
            }
            case ChannelType::App: {
                src.tone.reset();
                if (!c.appCapture) {
                    for (auto& cs : src.captures) cs->stop();
                    src.captures.clear();
                    src.pids.clear();
                    src.status = "session";
                    break;
                }
                // Capture each process tree root among matched sessions once.
                std::set<uint32_t> pids;
                for (auto* s : sessions_->matching(c.match))
                    if (!s->system) pids.insert(s->pid);
                if (!parentsLoaded && !pids.empty()) {
                    parents = parentPids();
                    parentsLoaded = true;
                }
                std::set<uint32_t> roots;
                for (uint32_t pid : pids) {
                    bool covered = false;
                    uint32_t cur = pid;
                    for (int depth = 0; depth < 32; ++depth) {
                        auto it = parents.find(cur);
                        if (it == parents.end() || it->second == 0 || it->second == cur) break;
                        cur = it->second;
                        if (pids.count(cur)) {
                            covered = true;
                            break;
                        }
                    }
                    if (!covered) roots.insert(pid);
                }
                std::erase_if(src.captures, [&](const std::shared_ptr<CaptureStream>& cs) {
                    if (roots.count(cs->pid())) return false;
                    cs->stop();
                    return true;
                });
                std::set<uint32_t> have;
                for (auto& cs : src.captures) have.insert(cs->pid());
                for (uint32_t pid : roots) {
                    if (have.count(pid)) continue;
                    std::string err;
                    auto cs = CaptureStream::openProcess(pid, buf, err);
                    if (cs) {
                        src.captures.push_back(cs);
                        log::info("App capture started for pid " + std::to_string(pid));
                    } else {
                        src.error = err;
                        log::warn("App capture failed for pid " + std::to_string(pid) + ": " + err);
                    }
                }
                src.pids = roots;
                src.status = src.captures.empty() ? (roots.empty() ? "offline" : "error") : "captured";
                break;
            }
        }
    }
    for (auto it = sources_.begin(); it != sources_.end();) {
        if (!live.count(it->first)) {
            for (auto& cs : it->second.captures) cs->stop();
            it = sources_.erase(it);
        } else {
            ++it;
        }
    }
}

void EngineService::reconcileOutputs() {
    std::set<std::string> wanted;
    std::string master;
    for (auto& b : profile().buses) {
        const std::string dev = resolveBusDevice(b);
        if (dev.empty()) continue;
        wanted.insert(dev);
        if (b.isMain) master = dev;
    }
    BufferSettings buf;
    buf.mode = config_.settings.bufferMode;
    if (buf.mode != audio_->bufferSettings().mode) {
        audio_->stopAll();
        audio_->setBufferSettings(buf);
    }
    audio_->reconcile(wanted, master);
}

void EngineService::compile() {
    ProfileConfig& p = profile();
    CompileContext ctx;
    ctx.sourcesFor = [this](const ChannelConfig& c) {
        std::vector<std::shared_ptr<PullSource>> v;
        auto it = sources_.find(c.id);
        if (it == sources_.end()) return v;
        if (it->second.tone) v.push_back(it->second.tone);
        for (auto& cs : it->second.captures)
            if (cs->status() == StreamStatus::Running) v.push_back(cs->stream());
        return v;
    };
    ctx.sendBlockedReason = [this](const ChannelConfig& c, const SendConfig& s, const BusConfig& b) {
        return sendBlockedReason(c, s, b);
    };
    ctx.outputFor = [this](const BusConfig& b) -> std::optional<OutputBinding> {
        const std::string dev = resolveBusDevice(b);
        if (dev.empty()) return std::nullopt;
        return audio_->binding(dev);
    };
    registry_.prune(p);
    registry_.applyParams(p);
    // App channel faders act on the Windows session volume; captured PCM is
    // already at that level, so the engine stage stays at unity.
    for (auto& c : p.channels)
        if (c.type == ChannelType::App) registry_.channel(c.id)->gain.setTarget(1.0f);
    mixer_.publish(compileGraph(p, registry_, ctx, mixer_.nextEpoch()));
    markStateDirty();
}

void EngineService::applySessionVolumes() {
    ProfileConfig& p = profile();
    for (auto& c : p.channels) {
        if (c.type != ChannelType::App) continue;
        float vol = std::min(c.volume, 1.0f);  // Windows session volume range
        bool mute = c.mute;
        if (const GroupConfig* g = p.findGroup(c.groupId)) {
            vol *= g->volume;
            mute = mute || g->mute;
        }
        auto r = sessions_->apply(c.match, vol, mute);
        if (r.changed) lastSessionApply_[c.id] = {vol, Clock::now()};
        else if (!lastSessionApply_.count(c.id)) lastSessionApply_[c.id] = {vol, Clock::now()};
    }
}

void EngineService::adoptExternalSessionChanges() {
    // If the user changed an app's level elsewhere (e.g. Windows Volume
    // Mixer), adopt it instead of fighting it.
    ProfileConfig& p = profile();
    for (auto& c : p.channels) {
        if (c.type != ChannelType::App) continue;
        auto matched = sessions_->matching(c.match);
        if (matched.empty()) continue;
        auto it = lastSessionApply_.find(c.id);
        if (it == lastSessionApply_.end()) continue;
        if (std::chrono::duration<double>(Clock::now() - it->second.second).count() < 1.0) continue;
        const float v0 = matched[0]->volume;
        const bool m0 = matched[0]->mute;
        const bool uniform = std::all_of(matched.begin(), matched.end(), [&](const SessionInfo* s) {
            return std::fabs(s->volume - v0) < 0.002f && s->mute == m0;
        });
        if (!uniform) continue;
        float groupVol = 1.0f;
        bool groupMute = false;
        if (const GroupConfig* g = p.findGroup(c.groupId)) {
            groupVol = g->volume;
            groupMute = g->mute;
        }
        const float expected = c.volume * groupVol;
        if (std::fabs(v0 - expected) > 0.005f && groupVol > 0.001f) {
            c.volume = std::clamp(v0 / groupVol, 0.0f, 1.0f);
            lastSessionApply_[c.id] = {v0, Clock::now()};
            markDirty();
        }
        if (!groupMute && m0 != c.mute) {
            c.mute = m0;
            markDirty();
        }
    }
}

// ---------------------------------------------------------------------------
// Mutation helper

// Coalescing key for a continuous edit: the target plus the fields it changes.
static std::string editKey(const std::string& prefix, const json& args) {
    std::string k = prefix;
    for (auto& [name, v] : args.items())
        if (name != "id") k += ":" + name;
    return k;
}

json EngineService::mutate(bool undoable, bool structural, const std::function<json(ProfileConfig&)>& fn,
                           const std::string& coalesceKey) {
    ProfileConfig& live = profile();
    ProfileConfig copy = live;
    json result = fn(copy);
    if (auto issue = validateRouting(copy, cablePairs())) throw ProtocolError("rejected", issue->message);
    if (undoable) history_.record([&] { return toJson(live); }, coalesceKey, UndoHistory::Clock::now());
    live = std::move(copy);
    markDirty();
    if (structural) {
        rebuildAudio();
        rebindEndpointVolumes();
    } else {
        applyLiveParams();
    }
    return result;
}

void EngineService::restoreSnapshot(const json& snap) {
    ProfileConfig restored = profileFromJson(snap, "");
    if (restored.id != profile().id) throw ProtocolError("rejected", "undo history belongs to another profile");
    normalizeProfile(restored);
    const bool sameShape = structuralView(restored) == structuralView(profile());
    profile() = std::move(restored);
    markDirty();
    if (sameShape) {
        applyLiveParams();  // levels, mutes, sends: instant, no audio gap
    } else {
        rebuildAudio();
        rebindEndpointVolumes();
    }
}

void EngineService::applyLiveParams() {
    ProfileConfig& live = profile();
    registry_.applyParams(live);
    for (auto& c : live.channels)
        if (c.type == ChannelType::App) registry_.channel(c.id)->gain.setTarget(1.0f);
    applySessionVolumes();
}

// ---------------------------------------------------------------------------
// IPC

void EngineService::handleMessage(uint64_t client, const std::string& line) {
    int64_t id = 0;
    json reply;
    try {
        Request req = parseRequest(line);
        id = req.id;
        if (!helloClients_.count(client) && req.cmd != "hello") {
            throw ProtocolError("bad_request", "first command must be 'hello'");
        }
        if (req.cmd == "hello") {
            const int ver = static_cast<int>(argFloat(req.args, "protocol", 0, 1000));
            if (ver != kProtocolVersion)
                throw ProtocolError("unsupported", "protocol version " + std::to_string(ver) + " not supported (engine speaks " +
                                                       std::to_string(kProtocolVersion) + ")");
            helloClients_.insert(client);
        }
        reply = okResponse(id, execute(req.cmd, req.args));
    } catch (const ProtocolError& e) {
        reply = errorResponse(id, e.code, e.what());
    } catch (const std::exception& e) {
        log::error(std::string("command failed: ") + e.what());
        reply = errorResponse(id, "internal", e.what());
    }
    if (pipe_) pipe_->send(client, reply.dump());
    // Push confirmed state promptly after mutations.
    if (stateDirty_ && pipe_) {
        lastStateSent_ = Clock::now();
        stateDirty_ = false;
        broadcastState();
    }
}

json EngineService::execute(const std::string& cmd, const json& args) {
    auto it = handlers_.find(cmd);
    if (it == handlers_.end()) throw ProtocolError("bad_request", "unknown command '" + cmd + "'");
    return it->second(args);
}

void EngineService::broadcastState() {
    if (!pipe_ || helloClients_.empty()) return;
    const std::string msg = eventMessage("state", buildState()).dump();
    for (auto c : helloClients_) pipe_->send(c, msg);
}

void EngineService::broadcastWindowsVolume() {
    if (!pipe_ || helloClients_.empty()) return;
    const std::string msg = eventMessage("windowsVolume", windowsVolumeJson()).dump();
    for (auto c : helloClients_) pipe_->send(c, msg);
}

void EngineService::broadcastMeters() {
    ProfileConfig& p = profile();
    json ch = json::object(), bu = json::object();
    for (auto& c : p.channels) {
        ChannelRuntime* rt = registry_.findChannel(c.id);
        if (!rt) continue;
        const bool managed = c.type != ChannelType::App || !sources_[c.id].captures.empty();
        if (!managed) {
            // Session-control channel: Windows-measured session peak.
            const float pk = sessions_->peak(c.match);
            ch[c.id] = {{"peakL", pk}, {"peakR", pk}, {"rmsL", 0}, {"rmsR", 0}, {"clip", false}, {"source", "session"}};
            continue;
        }
        MeterReading m = rt->meter.drain();
        ch[c.id] = {{"peakL", m.peakL}, {"peakR", m.peakR}, {"rmsL", m.rmsL}, {"rmsR", m.rmsR},
                    {"clip", m.clips > 0}, {"source", "engine"}};
    }
    for (auto& b : p.buses) {
        BusRuntime* rt = registry_.findBus(b.id);
        if (!rt) continue;
        MeterReading m = rt->meter.drain();
        bu[b.id] = {{"peakL", m.peakL}, {"peakR", m.peakR}, {"rmsL", m.rmsL}, {"rmsR", m.rmsR}, {"clip", m.clips > 0}};
    }
    if (!pipe_ || helloClients_.empty()) return;
    const std::string msg = eventMessage("meters", {{"channels", ch}, {"buses", bu}}).dump();
    for (auto c : helloClients_) pipe_->send(c, msg, true);
}

// ---------------------------------------------------------------------------
// State

std::string EngineService::channelStatus(const ChannelConfig& c) const {
    auto it = sources_.find(c.id);
    if (c.type == ChannelType::App) {
        auto matched = sessions_->matching(c.match);
        if (matched.empty()) return "offline";
        const bool active = std::any_of(matched.begin(), matched.end(), [](auto* s) { return s->state == "active"; });
        return active ? "active" : "idle";
    }
    if (it == sources_.end()) return "offline";
    return it->second.status;
}

json EngineService::channelRuntimeJson(const ChannelConfig& c) {
    ChannelSource& src = sources_[c.id];
    const bool managed = c.type != ChannelType::App || !src.captures.empty();
    json sessions = json::array();
    std::string icon;
    if (c.type == ChannelType::App)
        for (auto* s : sessions_->matching(c.match)) {
            sessions.push_back({{"pid", s->pid}, {"deviceId", s->deviceId}, {"deviceName", s->deviceName},
                                {"state", s->state}, {"volume", s->volume}, {"mute", s->mute}});
            if (icon.empty()) icon = s->exePath;
        }
    json sends = json::object();
    for (auto& s : c.sends) {
        const BusConfig* b = profile().findBus(s.busId);
        if (!b) continue;
        std::string state, reason;
        if (!s.enabled) state = "disabled";
        else if (!managed) {
            state = "session-only";
            reason = c.type == ChannelType::App
                         ? "dB-Deck controls this app's Windows volume but does not receive its audio. It keeps playing "
                           "on its own Windows output. Enable app capture to route it."
                         : "Source offline";
        } else if (std::string r = sendBlockedReason(c, s, *b); !r.empty()) {
            state = "blocked";
            reason = r;
        } else if (resolveBusDevice(*b).empty() && b->busSends.empty()) {
            state = "no-output";
            reason = "Bus '" + b->name + "' has no connected output device";
        } else {
            state = "active";
        }
        sends[s.busId] = {{"state", state}, {"reason", reason}};
    }
    json captures = json::array();
    for (auto& cs : src.captures)
        captures.push_back({{"pid", cs->pid()}, {"format", cs->format().describe()}, {"bufferMs", cs->bufferMs()},
                            {"status", streamStatusName(cs->status())}, {"stream", streamStatsJson(cs->stream()->stats())}});
    // Windows' per-app output preference ("" = follows the Windows default).
    std::string appOutput;
    bool appOutputKnown = false;
    if (c.type == ChannelType::App && appOutput_ && appOutput_->available())
        for (auto* s : sessions_->matching(c.match))
            if (!s->system) {
                appOutput = appOutput_->getOutput(s->pid);
                appOutputKnown = true;
                break;
            }
    return {{"status", channelStatus(c)},
            {"appOutputDeviceId", appOutputKnown ? json(appOutput) : json(nullptr)},
            {"capability", managed ? "managed" : (c.type == ChannelType::App ? "session" : "none")},
            {"error", src.error},
            {"sessions", sessions},
            {"iconKey", icon},
            {"sends", sends},
            {"captures", captures}};
}

json EngineService::busRuntimeJson(const BusConfig& b) {
    bool fallback = false;
    const std::string dev = resolveBusDevice(b, &fallback);
    std::string status;
    if (b.deviceId.empty() && b.deviceName.empty() && b.fallbackDeviceIds.empty()) status = "no-destination";
    else if (dev.empty()) status = "disconnected";
    else if (!audio_->running(dev)) status = "error";
    else status = fallback ? "fallback" : "connected";
    const DeviceInfo* d = findDevice(dev.empty() ? b.deviceId : dev);
    std::string error;
    for (auto& o : audio_->outputs())
        if (o.deviceId == (dev.empty() ? b.deviceId : dev)) error = o.error;
    double latency = 0;
    for (auto& o : audio_->outputs())
        if (o.deviceId == dev) latency = o.latencyMs + (o.hasRing ? o.ring.targetFrames * 1000.0 / kEngineSampleRate : 0);
    return {{"status", status},
            {"activeDeviceId", dev},
            {"activeDeviceName", d ? d->name : (b.deviceName.empty() ? b.deviceId : b.deviceName)},
            {"usingFallback", fallback},
            {"error", error},
            {"outputLatencyMs", latency}};
}

json EngineService::availableAppsJson() {
    ProfileConfig& p = profile();
    std::map<std::string, json> apps;
    for (auto& s : sessions_->sessions()) {
        if (s.system && !config_.settings.showSystemSounds) continue;
        bool taken = false;
        for (auto& c : p.channels)
            if (c.type == ChannelType::App) {
                if (s.system ? std::find(c.match.exeNames.begin(), c.match.exeNames.end(), "#system") != c.match.exeNames.end()
                             : matchesRule(c.match, {s.pid, s.exeName, s.exePath}))
                    taken = true;
            }
        if (taken || s.exeName.empty()) continue;
        auto& a = apps[s.exeName];
        if (a.is_null()) {
            a = {{"exeName", s.exeName}, {"displayName", s.displayName}, {"iconKey", s.exePath},
                 {"state", s.state},     {"pids", json::array()},      {"devices", json::array()},
                 {"system", s.system}};
        }
        a["pids"].push_back(s.pid);
        if (s.state == "active") a["state"] = "active";
        bool hasDev = false;
        for (auto& d : a["devices"]) hasDev |= d == s.deviceName;
        if (!hasDev) a["devices"].push_back(s.deviceName);
    }
    json out = json::array();
    for (auto& [k, v] : apps) out.push_back(v);
    std::stable_sort(out.begin(), out.end(), [](const json& a, const json& b) {
        return (a["state"] == "active") > (b["state"] == "active");
    });
    return out;
}

json EngineService::windowsVolumeJson() const {
    return {{"default", volumeStateJson(defaultVolume_->state())}, {"main", volumeStateJson(mainVolume_->state())}};
}

CablePairs EngineService::cablePairs() const {
    // A loopback cable is an adapter named as a cable (dB-Deck's driver,
    // "VB-Audio Virtual Cable", "Virtual Audio Cable", ...) exposing one
    // recording endpoint and one or more playback endpoints that feed it. Other virtual devices
    // (Voicemeeter, NVIDIA Broadcast, Voicemod) process audio rather than
    // echo it, so they are not paired.
    std::map<std::string, std::pair<std::vector<const DeviceInfo*>, std::vector<const DeviceInfo*>>> byAdapter;
    for (auto& d : deviceList_) {
        if (!isDbDeckDriverDevice(d) && toLowerAscii(d.adapterName).find("cable") == std::string::npos) continue;
        if (d.adapterName.empty()) continue;
        auto& slot = byAdapter[d.adapterName];
        (d.render ? slot.first : slot.second).push_back(&d);
    }
    CablePairs pairs;
    for (auto& [adapter, rc] : byAdapter)
        if (!rc.first.empty() && rc.second.size() == 1)
            for (auto* r : rc.first) pairs[rc.second[0]->id].push_back(r->id);
    return pairs;
}

json EngineService::driverStateJson() const {
    json own = json::array(), third = json::array();
    for (auto& d : deviceList_) {
        if (isDbDeckDriverDevice(d)) own.push_back(deviceJson(d));
        else if (d.likelyVirtual) third.push_back(deviceJson(d));
    }
    const bool installed = !own.empty();
    return {{"installed", installed},
            {"status", installed ? "installed" : "not-installed"},
            {"message",
             installed
                 ? "dB-Deck Virtual Cable is installed. Audio played into \"dB-Deck Virtual Input\" comes out of "
                   "\"dB-Deck Virtual Output\"."
                 : "The dB-Deck virtual audio driver is not installed. It is built from driver/windows/dbdeck-vad "
                   "but is currently only test-signed, so it can only be installed on a test machine with test "
                   "signing enabled (see docs/DRIVER_PLAN.md). Existing virtual cables (for example VB-Audio or "
                   "Voicemeeter endpoints) can be used as inputs and bus outputs instead."},
            {"endpoints", own},
            {"cablePairs", [&] {
                 json a = json::array();
                 for (auto& [cap, rens] : cablePairs())
                     for (auto& ren : rens) a.push_back({{"recording", cap}, {"playback", ren}});
                 return a;
             }()},
            {"thirdPartyVirtualEndpoints", third}};
}

json EngineService::diagnosticsJson() {
    MixerStats ms = mixer_.stats();
    json outputs = json::array();
    for (auto& o : audio_->outputs()) {
        const DeviceInfo* d = findDevice(o.deviceId);
        json j = {{"deviceId", o.deviceId}, {"deviceName", d ? d->name : o.deviceId}, {"status", o.status},
                  {"error", o.error},       {"master", o.master},                     {"latencyMs", o.latencyMs},
                  {"bufferFrames", o.bufferFrames}, {"periodFrames", o.periodFrames}, {"format", o.format},
                  {"glitches", o.glitches}};
        if (o.hasRing) j["ring"] = streamStatsJson(o.ring);
        outputs.push_back(j);
    }
    json inputs = json::array();
    for (auto& c : profile().channels) {
        auto it = sources_.find(c.id);
        if (it == sources_.end()) continue;
        for (auto& cs : it->second.captures) {
            const AdaptiveStreamStats st = cs->stream()->stats();
            inputs.push_back({{"channelId", c.id},
                              {"channelName", c.name},
                              {"kind", cs->kind() == CaptureStream::Kind::Process    ? "process-loopback"
                                       : cs->kind() == CaptureStream::Kind::Loopback ? "endpoint-loopback"
                                                                                     : "capture"},
                              {"pid", cs->pid()},
                              {"format", cs->format().describe()},
                              {"deviceBufferMs", cs->bufferMs()},
                              {"ringTargetMs", st.targetFrames * 1000.0 / cs->stream()->sourceRate()},
                              {"stream", streamStatsJson(st)},
                              {"status", streamStatusName(cs->status())}});
        }
    }
    // Graph overview for inspection.
    json graph = {{"channels", json::array()}, {"buses", json::array()}};
    BusOrder order = orderBuses(profile());
    for (auto& id : order.order) {
        const BusConfig* b = profile().findBus(id);
        graph["buses"].push_back({{"id", id}, {"name", b ? b->name : id}, {"device", b ? resolveBusDevice(*b) : ""}});
    }
    return {{"engineVersion", kEngineVersion},
            {"sampleRate", kEngineSampleRate},
            {"bufferMode", config_.settings.bufferMode},
            {"clock", audio_->clockName()},
            {"mixer", {{"blocks", ms.blocks}, {"frames", ms.frames}, {"lastBlockUs", ms.lastBlockUs}, {"maxBlockUs", ms.maxBlockUs}}},
            {"outputs", outputs},
            {"inputs", inputs},
            {"busOrder", graph["buses"]},
            {"ipcClients", pipe_ ? pipe_->clientCount() : 0},
            {"configDir", store_.directory().string()},
            {"uptimeSec", std::chrono::duration<double>(Clock::now() - started_).count()},
            {"latencyNote",
             "Latency figures are computed from negotiated buffer sizes and ring targets (estimates). See "
             "docs/TESTING.md for measured loopback results."}};
}

json EngineService::buildState() {
    ProfileConfig& p = profile();
    json profiles = json::array();
    for (auto& pr : config_.profiles) profiles.push_back({{"id", pr.id}, {"name", pr.name}});
    json chRuntime = json::object(), busRuntime = json::object();
    for (auto& c : p.channels) chRuntime[c.id] = channelRuntimeJson(c);
    for (auto& b : p.buses) busRuntime[b.id] = busRuntimeJson(b);
    json devices = json::array();
    for (auto& d : deviceList_) devices.push_back(deviceJson(d));
    bool healthy = true;
    for (auto& [id, b] : busRuntime.items())
        if (b["status"] == "error") healthy = false;
    json hotkeys = json::array();
    for (auto& k : config_.settings.hotkeys) hotkeys.push_back(toJson(k));
    // Devices another app holds exclusively (typically Voicemeeter).
    std::set<std::string> inUse;
    auto heldByOther = [](const json& rt) {
        return rt.contains("error") && rt["error"].is_string() &&
               rt["error"].get<std::string>().find("DEVICE_IN_USE") != std::string::npos;
    };
    for (auto& [id, b] : busRuntime.items())
        if (heldByOther(b)) inUse.insert(b.value("activeDeviceName", ""));
    for (auto& c : p.channels)
        if (heldByOther(chRuntime[c.id])) inUse.insert(c.deviceName.empty() ? c.name : c.deviceName);
    json conflicts = {{"voicemeeterRunning", !voicemeeter_.empty()}, {"devicesInUse", inUse}};
    return {{"engine",
             {{"version", kEngineVersion},
              {"protocol", kProtocolVersion},
              {"pid", GetCurrentProcessId()},
              {"clock", audio_->clockName()},
              {"sampleRate", kEngineSampleRate},
              {"healthy", healthy},
              {"masterMute", mixer_.masterMuted()},
              {"warnings", warnings_},
              {"undoDepth", history_.undoDepth()},
              {"redoDepth", history_.redoDepth()},
              {"solo", registry_.solo},
              {"conflicts", conflicts},
              {"iconsVersion", iconsVersion_}}},
            {"settings",
             {{"bufferMode", config_.settings.bufferMode},
              {"meterRateHz", config_.settings.meterRateHz},
              {"showSystemSounds", config_.settings.showSystemSounds},
              {"hotkeys", hotkeys}}},
            {"profiles", profiles},
            {"activeProfileId", p.id},
            {"profile", toJson(p)},
            {"runtime", {{"channels", chRuntime}, {"buses", busRuntime}}},
            {"devices", devices},
            {"availableApps", availableAppsJson()},
            {"windowsVolume", windowsVolumeJson()},
            {"driver", driverStateJson()}};
}

// ---------------------------------------------------------------------------
// Commands

void EngineService::registerCommands() {
    auto& h = handlers_;

    h["hello"] = [this](const json&) {
        return json{{"engine", "dB-Deck"},
                    {"version", kEngineVersion},
                    {"protocol", kProtocolVersion},
                    {"capabilities",
                     {{"sessionControl", true},
                      {"hardwareCapture", true},
                      {"endpointLoopback", true},
                      {"processLoopback", true},
                      {"managedRouting", true},
                      {"effects", json::array({"gain", "eq", "gate", "compressor", "limiter"})},
                      {"windowsMasterVolume", true},
                      {"appOutputDevice", appOutput_ && appOutput_->available()},
                      {"virtualDriver", false}}}};
    };
    h["getState"] = [this](const json&) { return buildState(); };
    h["getDiagnostics"] = [this](const json&) { return diagnosticsJson(); };
    h["getDriverState"] = [this](const json&) { return driverStateJson(); };
    h["getIcons"] = [this](const json&) {
        json icons = json::object();
        for (auto& path : knownIconPaths_) {
            const std::string icon = sessions_->iconFor(path);
            if (!icon.empty()) icons[path] = icon;
        }
        return json{{"version", iconsVersion_}, {"icons", icons}};
    };
    h["listDevices"] = [this](const json&) {
        json a = json::array();
        for (auto& d : deviceList_) a.push_back(deviceJson(d));
        return a;
    };
    h["listSessions"] = [this](const json&) {
        json a = json::array();
        for (auto& s : sessions_->sessions())
            a.push_back({{"pid", s.pid}, {"exeName", s.exeName}, {"displayName", s.displayName}, {"deviceName", s.deviceName},
                         {"state", s.state}, {"volume", s.volume}, {"mute", s.mute}, {"system", s.system}});
        return a;
    };

    // --- channels ---------------------------------------------------------
    h["createChannel"] = [this](const json& a) {
        const std::string typeName = argString(a, "type");
        auto type = channelTypeFromString(typeName);
        if (!type) throw ProtocolError("bad_args", "unknown channel type '" + typeName + "'");
        return mutate(true, true, [&](ProfileConfig& p) {
            std::string name = optString(a, "name").value_or("");
            ChannelConfig c = makeChannel(*type, name, p);
            switch (*type) {
                case ChannelType::App: {
                    const std::string exe = toLowerAscii(argString(a, "exeName"));
                    c.match = ruleForExe(exe);
                    for (auto& ch : p.channels)
                        if (ch.type == ChannelType::App && ch.match.exeNames == c.match.exeNames)
                            throw ProtocolError("rejected", "'" + ch.name + "' already controls " + exe);
                    if (name.empty())
                        for (auto& s : sessions_->sessions())
                            if (s.exeName == exe) name = s.displayName;
                    if (name.empty()) name = exe == "#system" ? "System Sounds" : exe;
                    break;
                }
                case ChannelType::HardwareInput:
                case ChannelType::EndpointLoopback: {
                    c.deviceId = argString(a, "deviceId");
                    const DeviceInfo* d = findDevice(c.deviceId);
                    if (!d) throw ProtocolError("not_found", "device not found");
                    const bool wantRender = *type == ChannelType::EndpointLoopback;
                    if (d->render != wantRender)
                        throw ProtocolError("bad_args", wantRender ? "loopback needs a playback device"
                                                                   : "hardware input needs a recording device");
                    c.deviceName = d->name;
                    if (name.empty()) name = d->name;
                    break;
                }
                case ChannelType::TestTone:
                    c.toneHz = optFloat(a, "toneHz", 20, 20000).value_or(440.0f);
                    if (name.empty()) name = "Test Tone " + std::to_string(static_cast<int>(c.toneHz)) + " Hz";
                    c.volume = 0.5f;  // start quietly
                    break;
            }
            c.name = name;
            if (auto g = optString(a, "groupId"); g && !g->empty()) {
                requireFound(p.findGroup(*g), "group");
                c.groupId = *g;
            }
            p.channels.push_back(c);
            return toJson(c);
        });
    };
    h["deleteChannel"] = [this](const json& a) {
        const std::string id = argString(a, "id");
        return mutate(true, true, [&](ProfileConfig& p) {
            requireFound(p.findChannel(id), "channel");
            std::erase_if(p.channels, [&](const ChannelConfig& c) { return c.id == id; });
            return json{{"deleted", id}};
        });
    };
    h["updateChannel"] = [this](const json& a) {
        const std::string id = argString(a, "id");
        // Continuous-parameter-only updates do not rebuild sources or streams.
        const bool structural = a.contains("appCapture") || a.contains("deviceId") || a.contains("exeNames") ||
                                a.contains("pathContains") || a.contains("groupId") || a.contains("toneHz");
        const bool discrete = structural || a.contains("name") || a.contains("color");
        return mutate(true, structural, [&](ProfileConfig& p) {
            ChannelConfig* c = p.findChannel(id);
            requireFound(c, "channel");
            if (auto v = optString(a, "name")) {
                if (v->empty()) throw ProtocolError("bad_args", "name cannot be empty");
                c->name = *v;
            }
            if (auto v = optFloat(a, "volume", 0, kFaderMax)) {
                // Windows session volume cannot exceed 100%; boost needs managed audio.
                c->volume = (c->type == ChannelType::App && !c->appCapture) ? std::min(*v, 1.0f) : *v;
            }
            if (auto v = optBool(a, "mute")) c->mute = *v;
            if (auto v = optFloat(a, "gainDb", -60, 24)) c->gainDb = *v;
            if (auto v = optFloat(a, "balance", -1, 1)) c->balance = *v;
            if (auto v = optFloat(a, "toneHz", 20, 20000)) c->toneHz = *v;
            if (auto v = optString(a, "groupId")) {
                if (!v->empty()) requireFound(p.findGroup(*v), "group");
                c->groupId = *v;
            }
            if (auto v = optBool(a, "appCapture")) {
                if (c->type != ChannelType::App) throw ProtocolError("bad_args", "appCapture applies to app channels");
                c->appCapture = *v;
            }
            if (auto v = optString(a, "deviceId")) {
                const DeviceInfo* d = findDevice(*v);
                requireFound(d, "device");
                c->deviceId = *v;
                c->deviceName = d->name;
            }
            if (a.contains("exeNames")) {
                if (!a["exeNames"].is_array()) throw ProtocolError("bad_args", "exeNames must be an array");
                c->match.exeNames.clear();
                for (auto& n : a["exeNames"]) {
                    if (!n.is_string()) throw ProtocolError("bad_args", "exeNames must be strings");
                    c->match.exeNames.push_back(toLowerAscii(n.get<std::string>()));
                }
                if (c->match.exeNames.empty()) throw ProtocolError("bad_args", "at least one exe name is required");
            }
            if (auto v = optString(a, "pathContains")) c->match.pathContains = *v;
            if (auto v = optString(a, "reconnect")) {
                if (*v != "auto" && *v != "manual") throw ProtocolError("bad_args", "reconnect must be auto|manual");
                c->reconnect = *v;
            }
            if (auto v = optString(a, "color")) {
                if (!isValidColor(*v)) throw ProtocolError("bad_args", "color must be \"#rrggbb\" or empty");
                c->color = *v;
            }
            return toJson(*c);
        }, discrete ? "" : editKey("channel:" + id, a));
    };
    h["reorderChannels"] = [this](const json& a) {
        if (!a.contains("ids") || !a["ids"].is_array()) throw ProtocolError("bad_args", "'ids' must be an array");
        return mutate(true, false, [&](ProfileConfig& p) {
            std::vector<ChannelConfig> ordered;
            for (auto& idj : a["ids"]) {
                if (!idj.is_string()) throw ProtocolError("bad_args", "ids must be strings");
                auto it = std::find_if(p.channels.begin(), p.channels.end(),
                                       [&](const ChannelConfig& c) { return c.id == idj.get<std::string>(); });
                if (it == p.channels.end()) continue;
                ordered.push_back(std::move(*it));
                p.channels.erase(it);
            }
            for (auto& rest : p.channels) ordered.push_back(std::move(rest));
            p.channels = std::move(ordered);
            return json{{"ok", true}};
        });
    };
    h["reorderBuses"] = [this](const json& a) {
        if (!a.contains("ids") || !a["ids"].is_array()) throw ProtocolError("bad_args", "'ids' must be an array");
        return mutate(true, false, [&](ProfileConfig& p) {
            std::vector<BusConfig> ordered;
            for (auto& idj : a["ids"]) {
                if (!idj.is_string()) throw ProtocolError("bad_args", "ids must be strings");
                auto it = std::find_if(p.buses.begin(), p.buses.end(),
                                       [&](const BusConfig& b) { return b.id == idj.get<std::string>(); });
                if (it == p.buses.end()) continue;
                ordered.push_back(std::move(*it));
                p.buses.erase(it);
            }
            for (auto& rest : p.buses) ordered.push_back(std::move(rest));
            // Main always stays first.
            std::stable_partition(ordered.begin(), ordered.end(), [](const BusConfig& b) { return b.isMain; });
            p.buses = std::move(ordered);
            return json{{"ok", true}};
        });
    };
    h["setSolo"] = [this](const json& a) {
        // Monitoring aid: not saved, not undoable.
        if (a.contains("clear") && a["clear"].is_boolean() && a["clear"].get<bool>()) {
            registry_.solo.clear();
        } else {
            const std::string id = argString(a, "id");
            requireFound(profile().findChannel(id), "channel");
            if (optBool(a, "solo").value_or(true)) registry_.solo.insert(id);
            else registry_.solo.erase(id);
        }
        applyLiveParams();
        markStateDirty();
        return json{{"solo", registry_.solo}};
    };
    h["setSend"] = [this](const json& a) {
        const std::string cid = argString(a, "channelId"), bid = argString(a, "busId");
        const bool structural = a.contains("enabled") || a.contains("preFader");
        return mutate(true, structural, [&](ProfileConfig& p) {
            ChannelConfig* c = p.findChannel(cid);
            requireFound(c, "channel");
            requireFound(p.findBus(bid), "bus");
            SendConfig* s = c->findSend(bid);
            if (!s) {
                c->sends.push_back(SendConfig{bid, false, 1.0f, false});
                s = &c->sends.back();
            }
            if (auto v = optBool(a, "enabled")) s->enabled = *v;
            if (auto v = optFloat(a, "gain", 0, 2)) s->gain = *v;
            if (auto v = optBool(a, "preFader")) s->preFader = *v;
            return json{{"busId", s->busId}, {"enabled", s->enabled}, {"gain", s->gain}, {"preFader", s->preFader}};
        }, structural ? "" : "send:" + cid + ">" + bid);
    };

    // --- groups -------------------------------------------------------------
    h["createGroup"] = [this](const json& a) {
        return mutate(true, false, [&](ProfileConfig& p) {
            GroupConfig g{newId(), optString(a, "name").value_or("Group"), 1.0f, false, false};
            p.groups.push_back(g);
            if (a.contains("channelIds") && a["channelIds"].is_array())
                for (auto& id : a["channelIds"])
                    if (id.is_string())
                        if (ChannelConfig* c = p.findChannel(id.get<std::string>())) c->groupId = g.id;
            return toJson(g);
        });
    };
    h["updateGroup"] = [this](const json& a) {
        const std::string id = argString(a, "id");
        // Collapsing is view state, not an edit.
        const bool edit = a.contains("name") || a.contains("volume") || a.contains("mute");
        return mutate(edit, false, [&](ProfileConfig& p) {
            GroupConfig* g = p.findGroup(id);
            requireFound(g, "group");
            if (auto v = optString(a, "name")) g->name = *v;
            if (auto v = optFloat(a, "volume", 0, 1)) g->volume = *v;
            if (auto v = optBool(a, "mute")) g->mute = *v;
            if (auto v = optBool(a, "collapsed")) g->collapsed = *v;
            return toJson(*g);
        }, a.contains("name") ? "" : editKey("group:" + id, a));
    };
    h["deleteGroup"] = [this](const json& a) {
        const std::string id = argString(a, "id");
        return mutate(true, false, [&](ProfileConfig& p) {
            requireFound(p.findGroup(id), "group");
            std::erase_if(p.groups, [&](const GroupConfig& g) { return g.id == id; });
            for (auto& c : p.channels)
                if (c.groupId == id) c.groupId.clear();
            return json{{"deleted", id}};
        });
    };

    // --- buses ----------------------------------------------------------------
    h["createBus"] = [this](const json& a) {
        return mutate(true, true, [&](ProfileConfig& p) {
            BusConfig b;
            b.id = newId();
            b.name = optString(a, "name").value_or("Bus");
            if (auto d = optString(a, "deviceId"); d && !d->empty()) {
                const DeviceInfo* dev = findDevice(*d);
                requireFound(dev, "device");
                if (!dev->render) throw ProtocolError("bad_args", "bus output must be a playback device");
                b.deviceId = *d;
                b.deviceName = dev->name;
            }
            p.buses.push_back(b);
            return toJson(b);
        });
    };
    h["updateBus"] = [this](const json& a) {
        const std::string id = argString(a, "id");
        const bool structural = a.contains("deviceId") || a.contains("fallbackDeviceIds");
        const bool discrete = structural || a.contains("name") || a.contains("color");
        return mutate(true, structural, [&](ProfileConfig& p) {
            BusConfig* b = p.findBus(id);
            requireFound(b, "bus");
            if (auto v = optString(a, "name")) {
                if (v->empty()) throw ProtocolError("bad_args", "name cannot be empty");
                b->name = *v;
            }
            if (auto v = optString(a, "color")) {
                if (!isValidColor(*v)) throw ProtocolError("bad_args", "color must be \"#rrggbb\" or empty");
                b->color = *v;
            }
            if (auto v = optFloat(a, "volume", 0, kFaderMax)) b->volume = *v;
            if (auto v = optBool(a, "mute")) b->mute = *v;
            if (auto v = optString(a, "deviceId")) {
                if (!v->empty()) {
                    const DeviceInfo* d = findDevice(*v);
                    requireFound(d, "device");
                    if (!d->render) throw ProtocolError("bad_args", "bus output must be a playback device");
                    b->deviceName = d->name;
                } else {
                    b->deviceName.clear();
                }
                b->deviceId = *v;
            }
            if (a.contains("fallbackDeviceIds")) {
                if (!a["fallbackDeviceIds"].is_array()) throw ProtocolError("bad_args", "fallbackDeviceIds must be an array");
                b->fallbackDeviceIds.clear();
                for (auto& f : a["fallbackDeviceIds"])
                    if (f.is_string() && f.get<std::string>() != b->deviceId) b->fallbackDeviceIds.push_back(f.get<std::string>());
            }
            return toJson(*b);
        }, discrete ? "" : editKey("bus:" + id, a));
    };
    h["deleteBus"] = [this](const json& a) {
        const std::string id = argString(a, "id");
        return mutate(true, true, [&](ProfileConfig& p) {
            BusConfig* b = p.findBus(id);
            requireFound(b, "bus");
            if (b->isMain) throw ProtocolError("rejected", "the Main bus cannot be deleted");
            std::erase_if(p.buses, [&](const BusConfig& x) { return x.id == id; });
            for (auto& c : p.channels) std::erase_if(c.sends, [&](const SendConfig& s) { return s.busId == id; });
            for (auto& x : p.buses) std::erase_if(x.busSends, [&](const SendConfig& s) { return s.busId == id; });
            return json{{"deleted", id}};
        });
    };
    h["setBusSend"] = [this](const json& a) {
        const std::string from = argString(a, "fromBusId"), to = argString(a, "toBusId");
        if (from == to) throw ProtocolError("rejected", "a bus cannot send to itself");
        return mutate(true, true, [&](ProfileConfig& p) {
            BusConfig* b = p.findBus(from);
            requireFound(b, "bus");
            requireFound(p.findBus(to), "bus");
            auto it = std::find_if(b->busSends.begin(), b->busSends.end(), [&](const SendConfig& s) { return s.busId == to; });
            if (it == b->busSends.end()) {
                b->busSends.push_back(SendConfig{to, false, 1.0f, false});
                it = b->busSends.end() - 1;
            }
            if (auto v = optBool(a, "enabled")) it->enabled = *v;
            if (auto v = optFloat(a, "gain", 0, 2)) it->gain = *v;
            if (auto v = optBool(a, "preFader")) it->preFader = *v;
            return json{{"busId", it->busId}, {"enabled", it->enabled}, {"gain", it->gain}};
        });
    };
    h["setMainOutput"] = [this](const json& a) {
        const std::string dev = argString(a, "deviceId");
        return mutate(true, true, [&](ProfileConfig& p) {
            BusConfig* m = p.mainBus();
            requireFound(m, "Main bus");
            if (!dev.empty()) {
                const DeviceInfo* d = findDevice(dev);
                requireFound(d, "device");
                if (!d->render) throw ProtocolError("bad_args", "Main output must be a playback device");
                m->deviceName = d->name;
            } else {
                m->deviceName.clear();
            }
            m->deviceId = dev;
            std::erase(m->fallbackDeviceIds, dev);
            return toJson(*m);
        });
    };

    // --- effects ------------------------------------------------------------
    auto effectsOf = [](ProfileConfig& p, const json& a) -> std::vector<EffectConfig>& {
        const std::string target = argString(a, "target"), id = argString(a, "id");
        if (target == "channel") {
            ChannelConfig* c = p.findChannel(id);
            requireFound(c, "channel");
            return c->effects;
        }
        if (target == "bus") {
            BusConfig* b = p.findBus(id);
            requireFound(b, "bus");
            return b->effects;
        }
        throw ProtocolError("bad_args", "target must be 'channel' or 'bus'");
    };
    h["addEffect"] = [this, effectsOf](const json& a) {
        const std::string type = argString(a, "type");
        auto t = effectTypeFromString(type);
        if (!t) throw ProtocolError("bad_args", "unknown effect type '" + type + "'");
        return mutate(true, true, [&](ProfileConfig& p) {
            auto& fx = effectsOf(p, a);
            EffectConfig e;
            e.id = newId();
            e.type = type;
            for (auto& spec : effectParamSpecs(*t)) e.params[spec.name] = spec.def;
            fx.push_back(e);
            return toJson(e);
        });
    };
    h["updateEffect"] = [this, effectsOf](const json& a) {
        const std::string eid = argString(a, "effectId");
        return mutate(true, false, [&](ProfileConfig& p) {
            auto& fx = effectsOf(p, a);
            auto it = std::find_if(fx.begin(), fx.end(), [&](const EffectConfig& e) { return e.id == eid; });
            if (it == fx.end()) throw ProtocolError("not_found", "effect not found");
            if (auto v = optBool(a, "enabled")) it->enabled = *v;
            if (a.contains("params")) {
                const json& params = argObject(a, "params");
                auto t = effectTypeFromString(it->type);
                for (auto& [k, v] : params.items()) {
                    if (!v.is_number()) throw ProtocolError("bad_args", "param '" + k + "' must be a number");
                    const ParamSpec* spec = nullptr;
                    for (auto& s : effectParamSpecs(*t))
                        if (k == s.name) spec = &s;
                    if (!spec) throw ProtocolError("bad_args", "unknown parameter '" + k + "' for " + it->type);
                    it->params[k] = std::clamp(v.get<float>(), spec->min, spec->max);
                }
            }
            return toJson(*it);
        }, a.contains("enabled") ? "" : "effect:" + eid);
    };
    h["removeEffect"] = [this, effectsOf](const json& a) {
        const std::string eid = argString(a, "effectId");
        return mutate(true, true, [&](ProfileConfig& p) {
            auto& fx = effectsOf(p, a);
            const size_t before = fx.size();
            std::erase_if(fx, [&](const EffectConfig& e) { return e.id == eid; });
            if (fx.size() == before) throw ProtocolError("not_found", "effect not found");
            return json{{"deleted", eid}};
        });
    };
    h["moveEffect"] = [this, effectsOf](const json& a) {
        const std::string eid = argString(a, "effectId");
        const int index = static_cast<int>(argFloat(a, "index", 0, 1000));
        return mutate(true, true, [&](ProfileConfig& p) {
            auto& fx = effectsOf(p, a);
            auto it = std::find_if(fx.begin(), fx.end(), [&](const EffectConfig& e) { return e.id == eid; });
            if (it == fx.end()) throw ProtocolError("not_found", "effect not found");
            EffectConfig e = *it;
            fx.erase(it);
            fx.insert(fx.begin() + std::min<size_t>(index, fx.size()), e);
            return json{{"ok", true}};
        });
    };
    h["getEffectTypes"] = [](const json&) {
        json out = json::array();
        for (auto t : allEffectTypes()) {
            json params = json::array();
            for (auto& s : effectParamSpecs(t))
                params.push_back({{"name", s.name}, {"min", s.min}, {"max", s.max}, {"default", s.def}, {"unit", s.unit}});
            out.push_back({{"type", effectTypeName(t)}, {"params", params}});
        }
        return out;
    };

    // --- profiles & scenes ------------------------------------------------------
    h["createProfile"] = [this](const json& a) {
        const std::string name = optString(a, "name").value_or("Profile");
        ProfileConfig p;
        if (optBool(a, "copyCurrent").value_or(false)) {
            p = profileFromJson(toJson(profile()), "");
            // Fresh IDs so the copy is independent.
            p.id = newId();
        } else {
            p = makeDefaultProfile(name);
            if (const BusConfig* m = profile().mainBus()) {
                p.mainBus()->deviceId = m->deviceId;
                p.mainBus()->deviceName = m->deviceName;
            }
        }
        p.name = name;
        config_.profiles.push_back(p);
        markDirty();
        return json{{"id", p.id}, {"name", p.name}};
    };
    h["renameProfile"] = [this](const json& a) {
        ProfileConfig* p = config_.findProfile(argString(a, "id"));
        requireFound(p, "profile");
        p->name = argString(a, "name");
        markDirty();
        return json{{"id", p->id}, {"name", p->name}};
    };
    h["deleteProfile"] = [this](const json& a) {
        const std::string id = argString(a, "id");
        requireFound(config_.findProfile(id), "profile");
        if (config_.profiles.size() == 1) throw ProtocolError("rejected", "the last profile cannot be deleted");
        if (id == profile().id) throw ProtocolError("rejected", "switch to another profile before deleting this one");
        std::erase_if(config_.profiles, [&](const ProfileConfig& p) { return p.id == id; });
        markDirty();
        return json{{"deleted", id}};
    };
    h["loadProfile"] = [this](const json& a) {
        const std::string id = argString(a, "id");
        ProfileConfig* p = config_.findProfile(id);
        requireFound(p, "profile");
        if (auto issue = validateRouting(*p, cablePairs())) throw ProtocolError("rejected", "profile has invalid routing: " + issue->message);
        config_.activeProfileId = id;
        history_.clear();
        markDirty();
        rebuildAudio();
        rebindEndpointVolumes();
        return json{{"activeProfileId", id}};
    };
    h["createScene"] = [this](const json& a) {
        const std::string name = optString(a, "name").value_or("Scene");
        return mutate(true, false, [&](ProfileConfig& p) {
            SceneConfig s = captureScene(p, newId(), name);
            p.scenes.push_back(s);
            return json{{"id", s.id}, {"name", s.name}};
        });
    };
    h["updateScene"] = [this](const json& a) {
        const std::string id = argString(a, "id");
        return mutate(true, false, [&](ProfileConfig& p) {
            SceneConfig* s = p.findScene(id);
            requireFound(s, "scene");
            const std::string name = optString(a, "name").value_or(s->name);
            if (optBool(a, "recapture").value_or(false)) *s = captureScene(p, id, name);
            s->name = name;
            return json{{"id", s->id}, {"name", s->name}};
        });
    };
    h["deleteScene"] = [this](const json& a) {
        const std::string id = argString(a, "id");
        return mutate(true, false, [&](ProfileConfig& p) {
            requireFound(p.findScene(id), "scene");
            std::erase_if(p.scenes, [&](const SceneConfig& s) { return s.id == id; });
            return json{{"deleted", id}};
        });
    };
    h["applyScene"] = [this](const json& a) {
        const std::string id = argString(a, "id");
        // Scenes only touch levels/mutes/send state. Send enable flags need a
        // graph recompile, but existing sources and device streams are reused.
        return mutate(true, true, [&](ProfileConfig& p) {
            SceneConfig* s = p.findScene(id);
            requireFound(s, "scene");
            applyScene(p, *s);
            return json{{"applied", id}};
        });
    };

    // --- global ---------------------------------------------------------------
    h["setMasterMute"] = [this](const json& a) {
        mixer_.setMasterMute(argBool(a, "mute"));
        markStateDirty();
        return json{{"masterMute", mixer_.masterMuted()}};
    };
    h["setWindowsVolume"] = [this](const json& a) {
        const std::string target = optString(a, "target").value_or("default");
        EndpointVolumeController* ctl = target == "default" ? defaultVolume_.get()
                                        : target == "main"  ? mainVolume_.get()
                                                            : nullptr;
        if (!ctl) throw ProtocolError("bad_args", "target must be 'default' or 'main'");
        std::string err;
        if (auto v = optFloat(a, "volume", 0, 1))
            if (!ctl->setVolume(*v, err)) throw ProtocolError("rejected", err);
        if (auto m = optBool(a, "mute"))
            if (!ctl->setMute(*m, err)) throw ProtocolError("rejected", err);
        if (auto s = optString(a, "step")) {
            if (*s != "up" && *s != "down") throw ProtocolError("bad_args", "step must be up|down");
            if (!ctl->stepVolume(*s == "up", err)) throw ProtocolError("rejected", err);
        }
        broadcastWindowsVolume();
        return volumeStateJson(ctl->state());
    };
    h["updateSettings"] = [this](const json& a) {
        bool rebuild = false;
        if (auto v = optString(a, "bufferMode")) {
            if (*v != "low" && *v != "normal" && *v != "safe") throw ProtocolError("bad_args", "bufferMode must be low|normal|safe");
            rebuild = *v != config_.settings.bufferMode;
            config_.settings.bufferMode = *v;
        }
        if (auto v = optFloat(a, "meterRateHz", 5, 60)) config_.settings.meterRateHz = static_cast<int>(*v);
        if (auto v = optBool(a, "showSystemSounds")) config_.settings.showSystemSounds = *v;
        if (a.contains("hotkeys")) {
            try {
                config_.settings.hotkeys = hotkeysFromJson(a["hotkeys"], "hotkeys");
            } catch (const ConfigError& e) {
                throw ProtocolError("bad_args", e.what());
            }
        }
        markDirty();
        if (rebuild) {
            // Buffer changes require reopening streams (brief, faded interruption).
            for (auto& [id, s] : sources_) {
                for (auto& c : s.captures) c->stop();
                s.captures.clear();
            }
            rebuildAudio();
        }
        return json{{"bufferMode", config_.settings.bufferMode}, {"meterRateHz", config_.settings.meterRateHz},
                    {"showSystemSounds", config_.settings.showSystemSounds}};
    };
    h["importVoicemeeter"] = [this](const json& a) {
        std::filesystem::path path;
        if (auto p = optString(a, "path"); p && !p->empty()) {
            path = std::filesystem::path(toWide(*p));
        } else {
            // Newest Voicemeeter*_LastSettings.xml in Documents\Voicemeeter.
            PWSTR docs = nullptr;
            std::filesystem::path dir;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) dir = std::filesystem::path(docs) / L"Voicemeeter";
            CoTaskMemFree(docs);
            std::error_code ec;
            std::filesystem::file_time_type newest{};
            for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
                const std::wstring n = e.path().filename().wstring();
                if (n.rfind(L"Voicemeeter", 0) == 0 && n.size() > 21 && n.ends_with(L"_LastSettings.xml") &&
                    (path.empty() || e.last_write_time() > newest)) {
                    path = e.path();
                    newest = e.last_write_time();
                }
            }
            if (path.empty()) throw ProtocolError("not_found", "No Voicemeeter settings were found in Documents\\Voicemeeter");
        }
        std::ifstream f(path, std::ios::binary);
        if (!f) throw ProtocolError("not_found", "Cannot read " + toUtf8(path.wstring()));
        std::ostringstream ss;
        ss << f.rdbuf();
        if (ss.str().size() > 8 * 1024 * 1024) throw ProtocolError("bad_args", "file is too large");

        std::vector<ImportDevice> devs;
        for (auto& d : deviceList_) devs.push_back({d.id, d.name, d.render});
        VoicemeeterImportResult r;
        try {
            r = importVoicemeeter(ss.str(), devs, "Voicemeeter import");
        } catch (const ConfigError& e) {
            throw ProtocolError("rejected", std::string("Could not import: ") + e.what());
        }
        r.profile.name = "Voicemeeter " + r.edition + " import";
        if (auto issue = validateRouting(r.profile, cablePairs()))
            throw ProtocolError("rejected", "The imported routing would create a loop: " + issue->message);
        config_.profiles.push_back(r.profile);
        markDirty();
        log::info("Imported " + toUtf8(path.wstring()) + " as profile '" + r.profile.name + "'");
        json result = {{"profileId", r.profile.id},
                       {"profileName", r.profile.name},
                       {"edition", r.edition},
                       {"source", toUtf8(path.wstring())},
                       {"channels", r.profile.channels.size()},
                       {"buses", r.profile.buses.size()},
                       {"notes", r.notes}};
        if (optBool(a, "activate").value_or(false)) execute("loadProfile", {{"id", r.profile.id}});
        return result;
    };
    h["setAppOutput"] = [this](const json& a) {
        const std::string id = argString(a, "channelId");
        const std::string dev = optString(a, "deviceId").value_or("");
        if (!appOutput_ || !appOutput_->available())
            throw ProtocolError("unsupported", appOutput_ ? appOutput_->unavailableReason() : "not available");
        const ChannelConfig* c = profile().findChannel(id);
        requireFound(c, "channel");
        if (c->type != ChannelType::App) throw ProtocolError("bad_args", "only app channels have an output device");
        if (!dev.empty()) {
            const DeviceInfo* d = findDevice(dev);
            requireFound(d, "device");
            if (!d->render || d->state != "active") throw ProtocolError("bad_args", "choose a connected playback device");
        }
        std::set<uint32_t> pids;
        for (auto* s : sessions_->matching(c->match))
            if (!s->system) pids.insert(s->pid);
        if (pids.empty())
            throw ProtocolError("rejected", "Start " + c->name +
                                                " and play something first. Windows sets an app's output while it is running "
                                                "and then remembers it.");
        std::string err;
        for (uint32_t pid : pids)
            if (!appOutput_->setOutput(pid, dev, err)) throw ProtocolError("rejected", err);
        sessionsDirty_ = true;  // sessions move to the new device
        markStateDirty();
        const DeviceInfo* d = findDevice(dev);
        log::info("App output for '" + c->name + "' set to " + (d ? d->name : "Windows default"));
        return json{{"channelId", id}, {"deviceId", dev}, {"processes", pids.size()}};
    };
    h["setLayout"] = [this](const json& a) {
        profile().layout = argObject(a, "layout");
        markDirty();
        return json{{"ok", true}};
    };
    h["undo"] = [this](const json&) {
        auto snap = history_.undo(toJson(profile()));
        if (!snap) throw ProtocolError("rejected", "nothing to undo");
        restoreSnapshot(*snap);
        return json{{"undoDepth", history_.undoDepth()}, {"redoDepth", history_.redoDepth()}};
    };
    h["redo"] = [this](const json&) {
        auto snap = history_.redo(toJson(profile()));
        if (!snap) throw ProtocolError("rejected", "nothing to redo");
        restoreSnapshot(*snap);
        return json{{"undoDepth", history_.undoDepth()}, {"redoDepth", history_.redoDepth()}};
    };
    h["closeVoicemeeter"] = [this](const json&) {
        // Only on the user's request: asks Voicemeeter to close like clicking X.
        auto vm = findVoicemeeter();
        if (vm.empty()) return json{{"wasRunning", false}, {"closeRequested", 0}};
        const int n = requestClose(vm);
        log::info("Asked Voicemeeter to close (" + std::to_string(n) + " windows)");
        lastProcessCheck_ = {};
        return json{{"wasRunning", true}, {"closeRequested", n}};
    };
    h["shutdown"] = [this](const json&) {
        quit_ = true;
        return json{{"stopping", true}};
    };
}

}  // namespace dbdeck::win
