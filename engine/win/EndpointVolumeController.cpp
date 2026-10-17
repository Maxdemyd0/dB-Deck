#include "EndpointVolumeController.h"

#include <wrl/implements.h>

#include <algorithm>
#include <cmath>

namespace dbdeck::win {

using namespace Microsoft::WRL;

namespace {

class VolumeCallback : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IAudioEndpointVolumeCallback> {
public:
    VolumeCallback(std::string deviceId, EndpointVolumeController::Notify notify)
        : deviceId_(std::move(deviceId)), notify_(std::move(notify)) {}

    STDMETHODIMP OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) override {
        if (!data) return E_POINTER;
        const bool ours = IsEqualGUID(data->guidEventContext, kDbDeckVolumeContext) != 0;
        if (notify_) notify_(deviceId_, data->fMasterVolume, data->bMuted != FALSE, ours);
        return S_OK;
    }

private:
    std::string deviceId_;
    EndpointVolumeController::Notify notify_;
};

}  // namespace

EndpointVolumeController::EndpointVolumeController(Notify notify) : notify_(std::move(notify)) {}

EndpointVolumeController::~EndpointVolumeController() { unbind(); }

void EndpointVolumeController::bind(IMMDevice* device, const std::string& deviceId, const std::string& name) {
    if (volume_ && state_.deviceId == deviceId) {
        std::lock_guard lock(mutex_);
        state_.deviceName = name;
        return;
    }
    unbind();
    EndpointVolumeState s;
    s.bound = true;
    s.deviceId = deviceId;
    s.deviceName = name;
    auto commit = [&] {
        std::lock_guard lock(mutex_);
        state_ = s;
    };
    if (!device) {
        s.unavailableReason = "Device is not available";
        commit();
        return;
    }
    HRESULT hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(volume_.ReleaseAndGetAddressOf()));
    if (FAILED(hr) || !volume_) {
        volume_.Reset();
        s.unavailableReason = "This endpoint has no software volume control (" + hrToString(hr) + ")";
        commit();
        return;
    }
    // Probe that the interface actually works: some endpoints (e.g. certain
    // digital/passthrough outputs) expose it but fail every call.
    float probe = 0.0f;
    hr = volume_->GetMasterVolumeLevelScalar(&probe);
    if (FAILED(hr)) {
        volume_.Reset();
        s.unavailableReason = "Endpoint volume is not functional (" + hrToString(hr) + ")";
        commit();
        return;
    }
    DWORD hw = 0;
    if (SUCCEEDED(volume_->QueryHardwareSupport(&hw))) {
        s.hardwareVolume = (hw & ENDPOINT_HARDWARE_SUPPORT_VOLUME) != 0;
        s.hardwareMute = (hw & ENDPOINT_HARDWARE_SUPPORT_MUTE) != 0;
    }
    float mn = 0, mx = 0, inc = 0;
    if (SUCCEEDED(volume_->GetVolumeRange(&mn, &mx, &inc))) {
        s.minDb = mn;
        s.maxDb = mx;
        s.incrementDb = inc;
    }
    UINT step = 0, steps = 0;
    if (SUCCEEDED(volume_->GetVolumeStepInfo(&step, &steps))) s.steps = steps;
    s.available = true;
    commit();
    refresh();

    callback_ = Make<VolumeCallback>(deviceId, notify_);
    if (FAILED(volume_->RegisterControlChangeNotify(callback_.Get())))
        callback_.Reset();  // still usable; the engine also re-reads periodically
}

void EndpointVolumeController::unbind() {
    if (volume_ && callback_) volume_->UnregisterControlChangeNotify(callback_.Get());
    callback_.Reset();
    volume_.Reset();
    std::lock_guard lock(mutex_);
    state_ = EndpointVolumeState{};
}

void EndpointVolumeController::refresh() {
    if (!volume_) return;
    float v = 0;
    BOOL m = FALSE;
    float db = 0;
    const bool okV = SUCCEEDED(volume_->GetMasterVolumeLevelScalar(&v));
    const bool okM = SUCCEEDED(volume_->GetMute(&m));
    const bool okD = SUCCEEDED(volume_->GetMasterVolumeLevel(&db));
    std::lock_guard lock(mutex_);
    if (okV) state_.volume = v;
    if (okM) state_.mute = m != FALSE;
    if (okD) state_.levelDb = db;
}

void EndpointVolumeController::onNotification(float volume, bool mute, bool fromDbDeck) {
    {
        std::lock_guard lock(mutex_);
        state_.volume = volume;
        state_.mute = mute;
        state_.lastChangeSource = fromDbDeck ? "dbdeck" : "external";
        ++state_.changeCount;
    }
    float db = 0;
    if (volume_ && SUCCEEDED(volume_->GetMasterVolumeLevel(&db))) {
        std::lock_guard lock(mutex_);
        state_.levelDb = db;
    }
}

bool EndpointVolumeController::setVolume(float scalar, std::string& error) {
    if (!volume_) {
        error = state_.unavailableReason.empty() ? "No endpoint bound" : state_.unavailableReason;
        return false;
    }
    scalar = std::clamp(scalar, 0.0f, 1.0f);
    {
        std::lock_guard lock(mutex_);
        if (std::fabs(state_.volume - scalar) < 0.0005f) return true;  // redundant: skip
    }
    HRESULT hr = volume_->SetMasterVolumeLevelScalar(scalar, &kDbDeckVolumeContext);
    if (FAILED(hr)) {
        error = "SetMasterVolumeLevelScalar failed: " + hrToString(hr);
        return false;
    }
    refresh();
    return true;
}

bool EndpointVolumeController::setMute(bool mute, std::string& error) {
    if (!volume_) {
        error = state_.unavailableReason.empty() ? "No endpoint bound" : state_.unavailableReason;
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        if (state_.mute == mute) return true;
    }
    HRESULT hr = volume_->SetMute(mute ? TRUE : FALSE, &kDbDeckVolumeContext);
    if (FAILED(hr)) {
        error = "SetMute failed: " + hrToString(hr);
        return false;
    }
    refresh();
    return true;
}

bool EndpointVolumeController::stepVolume(bool up, std::string& error) {
    if (!volume_) {
        error = "No endpoint bound";
        return false;
    }
    HRESULT hr = up ? volume_->VolumeStepUp(&kDbDeckVolumeContext) : volume_->VolumeStepDown(&kDbDeckVolumeContext);
    if (FAILED(hr)) {
        error = hrToString(hr);
        return false;
    }
    refresh();
    return true;
}

EndpointVolumeState EndpointVolumeController::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

}  // namespace dbdeck::win
