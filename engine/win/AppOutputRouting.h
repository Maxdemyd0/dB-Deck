#pragma once
// Per-application output device ("App volume and device preferences").
//
// Windows exposes this only through the undocumented WinRT factory
// "Windows.Media.Internal.AudioPolicyConfig" (IAudioPolicyConfigFactory),
// the same interface the Settings app uses. Its IID changed in build 21390;
// both are tried. Windows persists the choice per executable path, so it
// survives app restarts. If the interface is unavailable on a future build,
// available() is false and the feature is hidden: nothing is faked.

#include <string>

namespace dbdeck::win {

class AppOutputRouting {
public:
    AppOutputRouting();
    ~AppOutputRouting();
    AppOutputRouting(const AppOutputRouting&) = delete;
    AppOutputRouting& operator=(const AppOutputRouting&) = delete;

    bool available() const { return factory_ != nullptr; }
    const std::string& unavailableReason() const { return reason_; }

    // deviceId: IMMDevice endpoint ID of a playback device, or "" for the
    // Windows default. Applies to both the console and multimedia roles.
    bool setOutput(unsigned pid, const std::string& deviceId, std::string& error);
    // Returns the persisted endpoint ID for the process ("" = default).
    std::string getOutput(unsigned pid);

private:
    void* factory_ = nullptr;  // IAudioPolicyConfigFactory*
    bool newLayout_ = true;
    std::string reason_;
};

}  // namespace dbdeck::win
