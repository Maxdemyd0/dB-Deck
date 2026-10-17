#pragma once
// Windows audio session discovery and control on every active playback
// endpoint. Session volume (ISimpleAudioVolume) changes the application's own
// level inside Windows; it does NOT give dB-Deck the application's samples.

#include "ComUtil.h"
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "DeviceManager.h"
#include "IconCache.h"
#include "graph/AppMatch.h"

namespace dbdeck::win {

struct SessionInfo {
    std::string key;          // session instance identifier
    uint32_t pid = 0;
    std::string exeName;      // lower-case
    std::string exePath;
    std::string displayName;  // friendly application name
    std::string deviceId;     // endpoint the session plays on
    std::string deviceName;
    bool system = false;      // "System Sounds"
    std::string state;        // "active" | "inactive"
    float volume = 1.0f;
    bool mute = false;
};

class SessionManager {
public:
    SessionManager(DeviceManager& devices, std::function<void()> onSessionCreated);
    ~SessionManager();

    // Re-enumerates sessions on the given active render endpoints.
    void refresh(const std::vector<DeviceInfo>& devices, uint32_t ownPid);
    const std::vector<SessionInfo>& sessions() const { return list_; }

    struct ApplyResult {
        int matched = 0;
        int changed = 0;
    };
    ApplyResult apply(const AppMatchRule& rule, float volume, bool mute);
    float peak(const AppMatchRule& rule);  // max session peak (Windows-measured, post session volume)
    std::vector<const SessionInfo*> matching(const AppMatchRule& rule) const;
    std::string iconFor(const std::string& exePath) { return icons_.iconFor(toWide(exePath)); }

    void shutdown();

private:
    struct Entry {
        SessionInfo info;
        ComPtr<IAudioSessionControl2> control;
        ComPtr<ISimpleAudioVolume> volume;
        ComPtr<IAudioMeterInformation> meter;
    };
    struct Endpoint {
        ComPtr<IAudioSessionManager2> manager;
        ComPtr<IAudioSessionNotification> notification;
        std::string name;
    };

    ProcessIdentity processInfo(uint32_t pid);
    std::string friendlyName(const std::string& exePath, const std::string& exeName);

    DeviceManager& devices_;
    std::function<void()> onCreated_;
    std::map<std::string, Endpoint> endpoints_;
    std::map<std::string, Entry> entries_;
    std::vector<SessionInfo> list_;
    std::map<std::string, std::string> descriptions_;
    IconCache icons_;
};

}  // namespace dbdeck::win
