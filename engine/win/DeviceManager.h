#pragma once
// Endpoint enumeration and IMMNotificationClient device-change monitoring.

#include "ComUtil.h"
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "audio/Format.h"

namespace dbdeck::win {

struct DeviceInfo {
    std::string id;           // stable IMMDevice endpoint ID
    std::string name;         // e.g. "Speakers (Realtek(R) Audio)"
    std::string adapterName;  // e.g. "Realtek(R) Audio"
    bool render = true;
    std::string state;        // "active" | "unplugged" | "disabled" | "notpresent"
    bool isDefault = false;         // default console device for its flow
    bool isDefaultComms = false;    // default communications device
    bool likelyVirtual = false;     // name-based heuristic (VB-Audio, Voicemeeter, etc.)
    bool formatKnown = false;
    DeviceFormat mixFormat;
};

enum class DeviceEventKind { Added, Removed, StateChanged, DefaultChanged, PropertyChanged };

struct DeviceEvent {
    DeviceEventKind kind;
    std::string deviceId;
    bool render = true;  // for DefaultChanged
};

DeviceFormat parseWaveFormat(const WAVEFORMATEX* wf);

class DeviceManager {
public:
    using Callback = std::function<void(const DeviceEvent&)>;  // invoked on arbitrary COM threads

    DeviceManager();
    ~DeviceManager();

    bool init(Callback cb, std::string& error);
    void shutdown();

    std::vector<DeviceInfo> enumerate();
    ComPtr<IMMDevice> device(const std::string& id);
    std::string defaultDeviceId(bool render);
    IMMDeviceEnumerator* enumerator() const { return enumerator_.Get(); }

private:
    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<IMMNotificationClient> client_;
};

}  // namespace dbdeck::win
