#include "SessionManager.h"

#include <wrl/implements.h>

#include <algorithm>
#include <filesystem>
#include <vector>

#include "EndpointVolumeController.h"

#pragma comment(lib, "version.lib")

namespace dbdeck::win {

using namespace Microsoft::WRL;

namespace {

class SessionCreatedNotification
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IAudioSessionNotification> {
public:
    explicit SessionCreatedNotification(std::function<void()> cb) : cb_(std::move(cb)) {}
    STDMETHODIMP OnSessionCreated(IAudioSessionControl*) override {
        if (cb_) cb_();  // enqueue only
        return S_OK;
    }

private:
    std::function<void()> cb_;
};

}  // namespace

SessionManager::SessionManager(DeviceManager& devices, std::function<void()> onSessionCreated)
    : devices_(devices), onCreated_(std::move(onSessionCreated)) {}

SessionManager::~SessionManager() { shutdown(); }

void SessionManager::shutdown() {
    for (auto& [id, ep] : endpoints_)
        if (ep.manager && ep.notification) ep.manager->UnregisterSessionNotification(ep.notification.Get());
    endpoints_.clear();
    entries_.clear();
    list_.clear();
}

ProcessIdentity SessionManager::processInfo(uint32_t pid) {
    ProcessIdentity p;
    p.pid = pid;
    UniqueHandle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!h) return p;
    wchar_t path[MAX_PATH * 2];
    DWORD size = static_cast<DWORD>(std::size(path));
    if (QueryFullProcessImageNameW(h.get(), 0, path, &size)) {
        p.exePath = toUtf8(path);
        p.exeName = toLowerAscii(toUtf8(std::filesystem::path(path).filename().wstring()));
    }
    return p;
}

std::string SessionManager::friendlyName(const std::string& exePath, const std::string& exeName) {
    if (auto it = descriptions_.find(exePath); it != descriptions_.end()) return it->second;
    std::string name;
    const std::wstring wpath = toWide(exePath);
    DWORD dummy = 0;
    const DWORD size = exePath.empty() ? 0 : GetFileVersionInfoSizeW(wpath.c_str(), &dummy);
    if (size) {
        std::vector<unsigned char> data(size);
        if (GetFileVersionInfoW(wpath.c_str(), 0, size, data.data())) {
            struct Translation {
                WORD lang, codepage;
            }* tr = nullptr;
            UINT len = 0;
            if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr), &len) &&
                len >= sizeof(Translation)) {
                wchar_t key[64];
                swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr->lang, tr->codepage);
                wchar_t* desc = nullptr;
                UINT dlen = 0;
                if (VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&desc), &dlen) && dlen > 1)
                    name = toUtf8(desc);
            }
        }
    }
    if (name.empty() && !exeName.empty()) {
        name = exeName.substr(0, exeName.rfind('.'));
        if (!name.empty()) name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    }
    descriptions_[exePath] = name;
    return name;
}

void SessionManager::refresh(const std::vector<DeviceInfo>& devices, uint32_t ownPid) {
    // Track session managers for every active render endpoint.
    std::set<std::string> active;
    for (auto& d : devices) {
        if (!d.render || d.state != "active") continue;
        active.insert(d.id);
        auto it = endpoints_.find(d.id);
        if (it != endpoints_.end()) {
            it->second.name = d.name;
            continue;
        }
        ComPtr<IMMDevice> dev = devices_.device(d.id);
        if (!dev) continue;
        Endpoint ep;
        ep.name = d.name;
        if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void**>(ep.manager.GetAddressOf()))))
            continue;
        // GetSessionEnumerator must be called once before notifications are delivered.
        ComPtr<IAudioSessionEnumerator> warm;
        ep.manager->GetSessionEnumerator(&warm);
        ep.notification = Make<SessionCreatedNotification>(onCreated_);
        if (FAILED(ep.manager->RegisterSessionNotification(ep.notification.Get()))) ep.notification.Reset();
        endpoints_.emplace(d.id, std::move(ep));
    }
    for (auto it = endpoints_.begin(); it != endpoints_.end();) {
        if (!active.count(it->first)) {
            if (it->second.manager && it->second.notification)
                it->second.manager->UnregisterSessionNotification(it->second.notification.Get());
            it = endpoints_.erase(it);
        } else {
            ++it;
        }
    }

    std::map<std::string, Entry> next;
    for (auto& [devId, ep] : endpoints_) {
        ComPtr<IAudioSessionEnumerator> en;
        if (FAILED(ep.manager->GetSessionEnumerator(&en))) continue;
        int count = 0;
        en->GetCount(&count);
        for (int i = 0; i < count; ++i) {
            ComPtr<IAudioSessionControl> c;
            if (FAILED(en->GetSession(i, &c))) continue;
            ComPtr<IAudioSessionControl2> c2;
            if (FAILED(c.As(&c2))) continue;
            AudioSessionState st = AudioSessionStateInactive;
            c2->GetState(&st);
            if (st == AudioSessionStateExpired) continue;
            CoTaskPtr<wchar_t> inst;
            if (FAILED(c2->GetSessionInstanceIdentifier(inst.put())) || !inst) continue;
            const std::string key = toUtf8(inst.get());

            Entry e;
            if (auto old = entries_.find(key); old != entries_.end()) e = old->second;
            else {
                e.control = c2;
                c.As(&e.volume);
                c.As(&e.meter);
                DWORD pid = 0;
                c2->GetProcessId(&pid);
                e.info.key = key;
                e.info.pid = pid;
                e.info.system = c2->IsSystemSoundsSession() == S_OK;
                if (e.info.system) {
                    e.info.displayName = "System Sounds";
                    e.info.exeName = "#system";
                } else {
                    ProcessIdentity p = processInfo(pid);
                    e.info.exeName = p.exeName;
                    e.info.exePath = p.exePath;
                    CoTaskPtr<wchar_t> dn;
                    std::string sessionName;
                    if (SUCCEEDED(c2->GetDisplayName(dn.put())) && dn && dn.get()[0] != L'@')
                        sessionName = toUtf8(dn.get());
                    e.info.displayName = !sessionName.empty() ? sessionName : friendlyName(p.exePath, p.exeName);
                    if (e.info.displayName.empty()) e.info.displayName = "Process " + std::to_string(pid);
                }
            }
            if (e.info.pid == ownPid && !e.info.system) continue;  // never list dB-Deck itself
            e.info.deviceId = devId;
            e.info.deviceName = ep.name;
            e.info.state = st == AudioSessionStateActive ? "active" : "inactive";
            if (e.volume) {
                float v = 1;
                BOOL m = FALSE;
                if (SUCCEEDED(e.volume->GetMasterVolume(&v))) e.info.volume = v;
                if (SUCCEEDED(e.volume->GetMute(&m))) e.info.mute = m != FALSE;
            }
            next.emplace(key, std::move(e));
        }
    }
    entries_ = std::move(next);
    list_.clear();
    for (auto& [k, e] : entries_) list_.push_back(e.info);
    std::sort(list_.begin(), list_.end(), [](const SessionInfo& a, const SessionInfo& b) {
        if (a.state != b.state) return a.state == "active";
        return a.displayName < b.displayName;
    });
}

std::vector<const SessionInfo*> SessionManager::matching(const AppMatchRule& rule) const {
    std::vector<const SessionInfo*> out;
    for (auto& [k, e] : entries_)
        if (!e.info.system && matchesRule(rule, {e.info.pid, e.info.exeName, e.info.exePath})) out.push_back(&e.info);
        else if (e.info.system && std::find(rule.exeNames.begin(), rule.exeNames.end(), "#system") != rule.exeNames.end())
            out.push_back(&e.info);
    return out;
}

SessionManager::ApplyResult SessionManager::apply(const AppMatchRule& rule, float volume, bool mute) {
    ApplyResult r;
    for (auto& [k, e] : entries_) {
        const bool isMatch = e.info.system
                                 ? std::find(rule.exeNames.begin(), rule.exeNames.end(), "#system") != rule.exeNames.end()
                                 : matchesRule(rule, {e.info.pid, e.info.exeName, e.info.exePath});
        if (!isMatch || !e.volume) continue;
        ++r.matched;
        float cur = 0;
        BOOL curMute = FALSE;
        e.volume->GetMasterVolume(&cur);
        e.volume->GetMute(&curMute);
        if (std::fabs(cur - volume) > 0.0005f) {
            if (SUCCEEDED(e.volume->SetMasterVolume(volume, &kDbDeckVolumeContext))) {
                e.info.volume = volume;
                ++r.changed;
            }
        }
        if ((curMute != FALSE) != mute) {
            if (SUCCEEDED(e.volume->SetMute(mute ? TRUE : FALSE, &kDbDeckVolumeContext))) {
                e.info.mute = mute;
                ++r.changed;
            }
        }
    }
    if (r.changed)
        for (auto& s : list_)
            if (auto it = entries_.find(s.key); it != entries_.end()) s = it->second.info;
    return r;
}

float SessionManager::peak(const AppMatchRule& rule) {
    float p = 0;
    for (auto& [k, e] : entries_) {
        if (!e.meter) continue;
        const bool isMatch = e.info.system
                                 ? std::find(rule.exeNames.begin(), rule.exeNames.end(), "#system") != rule.exeNames.end()
                                 : matchesRule(rule, {e.info.pid, e.info.exeName, e.info.exePath});
        if (!isMatch) continue;
        float v = 0;
        if (SUCCEEDED(e.meter->GetPeakValue(&v))) p = std::max(p, v);
    }
    return p;
}

}  // namespace dbdeck::win
