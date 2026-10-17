#pragma once
// Windows endpoint master volume (IAudioEndpointVolume) with change
// notifications. This is the same control Windows Quick Settings and the
// keyboard volume keys operate on, so changes made anywhere are reflected.
//
// Loop prevention: every change dB-Deck makes carries kDbDeckVolumeContext as
// its event context. The notification callback reports whether a change was
// ours, and the engine never issues a volume command in response to a
// notification, so there is no echo path. Redundant sets (same value) are
// skipped before reaching Windows.

#include "ComUtil.h"
#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include <functional>
#include <mutex>
#include <string>


namespace dbdeck::win {

// {7B4C0C1E-5D2A-4E8B-9C61-0D8B5E1A2F43}
inline constexpr GUID kDbDeckVolumeContext = {0x7b4c0c1e, 0x5d2a, 0x4e8b, {0x9c, 0x61, 0x0d, 0x8b, 0x5e, 0x1a, 0x2f, 0x43}};

struct EndpointVolumeState {
    bool bound = false;
    std::string deviceId;
    std::string deviceName;
    bool available = false;          // a working software volume interface exists
    std::string unavailableReason;
    float volume = 0.0f;             // Windows "volume level scalar" 0..1 (what Quick Settings shows)
    bool mute = false;
    float levelDb = 0.0f;
    float minDb = 0.0f, maxDb = 0.0f, incrementDb = 0.0f;
    unsigned steps = 0;
    bool hardwareVolume = false;     // endpoint implements volume in hardware
    bool hardwareMute = false;
    std::string lastChangeSource;    // "dbdeck" | "external" | ""
    uint64_t changeCount = 0;
};

class EndpointVolumeController {
public:
    // Called on a COM worker thread for every endpoint volume/mute change.
    using Notify = std::function<void(const std::string& deviceId, float volume, bool mute, bool fromDbDeck)>;

    explicit EndpointVolumeController(Notify notify);
    ~EndpointVolumeController();
    EndpointVolumeController(const EndpointVolumeController&) = delete;
    EndpointVolumeController& operator=(const EndpointVolumeController&) = delete;

    // Control thread.
    void bind(IMMDevice* device, const std::string& deviceId, const std::string& name);
    void unbind();
    bool setVolume(float scalar, std::string& error);
    bool setMute(bool mute, std::string& error);
    bool stepVolume(bool up, std::string& error);
    // Re-reads volume/mute from Windows (also applies notification data).
    void refresh();
    void onNotification(float volume, bool mute, bool fromDbDeck);
    EndpointVolumeState state() const;
    const std::string& deviceId() const { return state_.deviceId; }

private:
    Notify notify_;
    ComPtr<IAudioEndpointVolume> volume_;
    ComPtr<IAudioEndpointVolumeCallback> callback_;
    mutable std::mutex mutex_;  // guards state_ against the notification thread
    EndpointVolumeState state_;
};

}  // namespace dbdeck::win
