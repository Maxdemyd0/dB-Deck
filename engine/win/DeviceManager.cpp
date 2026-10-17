#include "DeviceManager.h"

#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/implements.h>

#include <algorithm>

#include "graph/AppMatch.h"

namespace dbdeck::win {

using namespace Microsoft::WRL;

// PKEY_AudioEngine_DeviceFormat (mmdeviceapi.h declares it without a definition in any import library).
static const PROPERTYKEY kPkeyDeviceFormat = {{0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};

DeviceFormat parseWaveFormat(const WAVEFORMATEX* wf) {
    DeviceFormat f;
    if (!wf) {
        f.type = SampleType::Unknown;
        return f;
    }
    f.sampleRate = wf->nSamplesPerSec;
    f.channels = wf->nChannels;
    f.containerBytes = static_cast<uint16_t>(wf->wBitsPerSample / 8);
    f.validBits = wf->wBitsPerSample;
    f.channelMask = 0;
    bool isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool isPcm = wf->wFormatTag == WAVE_FORMAT_PCM;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
        auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        f.channelMask = ext->dwChannelMask;
        f.validBits = ext->Samples.wValidBitsPerSample ? ext->Samples.wValidBitsPerSample : wf->wBitsPerSample;
        isFloat = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
        isPcm = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != 0;
    }
    if (isFloat && wf->wBitsPerSample == 32) f.type = SampleType::Float32;
    else if (isPcm && wf->wBitsPerSample == 16) f.type = SampleType::Int16;
    else if (isPcm && wf->wBitsPerSample == 24) f.type = SampleType::Int24;
    else if (isPcm && wf->wBitsPerSample == 32) f.type = f.validBits == 24 ? SampleType::Int24 : SampleType::Int32;
    else f.type = SampleType::Unknown;
    return f;
}

namespace {

class NotificationClient : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMMNotificationClient> {
public:
    explicit NotificationClient(DeviceManager::Callback cb) : cb_(std::move(cb)) {}

    STDMETHODIMP OnDeviceStateChanged(LPCWSTR id, DWORD) override {
        emit({DeviceEventKind::StateChanged, toUtf8(id)});
        return S_OK;
    }
    STDMETHODIMP OnDeviceAdded(LPCWSTR id) override {
        emit({DeviceEventKind::Added, toUtf8(id)});
        return S_OK;
    }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR id) override {
        emit({DeviceEventKind::Removed, toUtf8(id)});
        return S_OK;
    }
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id) override {
        if (role == eConsole) emit({DeviceEventKind::DefaultChanged, toUtf8(id), flow == eRender});
        return S_OK;
    }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR id, const PROPERTYKEY key) override {
        if (key == PKEY_Device_FriendlyName || key == kPkeyDeviceFormat)
            emit({DeviceEventKind::PropertyChanged, toUtf8(id)});
        return S_OK;
    }

private:
    void emit(const DeviceEvent& e) {
        // Must not block: the callback only enqueues work for the control thread.
        if (cb_) cb_(e);
    }
    DeviceManager::Callback cb_;
};

bool looksVirtual(const std::string& name, const std::string& adapter) {
    static const char* kHints[] = {"vb-audio", "voicemeeter", "vaio", "cable", "virtual", "voicemod",
                                   "broadcast", "steam streaming", "sonar", "wave link", "obs"};
    const std::string s = toLowerAscii(name + " " + adapter);
    return std::any_of(std::begin(kHints), std::end(kHints), [&](const char* h) { return s.find(h) != std::string::npos; });
}

}  // namespace

DeviceManager::DeviceManager() = default;
DeviceManager::~DeviceManager() { shutdown(); }

bool DeviceManager::init(Callback cb, std::string& error) {
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator_));
    if (FAILED(hr)) {
        error = "MMDeviceEnumerator: " + hrToString(hr);
        return false;
    }
    client_ = Make<NotificationClient>(std::move(cb));
    hr = enumerator_->RegisterEndpointNotificationCallback(client_.Get());
    if (FAILED(hr)) {
        error = "RegisterEndpointNotificationCallback: " + hrToString(hr);
        client_.Reset();
        return false;
    }
    return true;
}

void DeviceManager::shutdown() {
    if (enumerator_ && client_) enumerator_->UnregisterEndpointNotificationCallback(client_.Get());
    client_.Reset();
    enumerator_.Reset();
}

ComPtr<IMMDevice> DeviceManager::device(const std::string& id) {
    ComPtr<IMMDevice> d;
    if (enumerator_ && !id.empty()) enumerator_->GetDevice(toWide(id).c_str(), &d);
    return d;
}

std::string DeviceManager::defaultDeviceId(bool render) {
    ComPtr<IMMDevice> d;
    if (!enumerator_ || FAILED(enumerator_->GetDefaultAudioEndpoint(render ? eRender : eCapture, eConsole, &d)))
        return {};
    CoTaskPtr<wchar_t> id;
    if (FAILED(d->GetId(id.put()))) return {};
    return toUtf8(id.get());
}

std::vector<DeviceInfo> DeviceManager::enumerate() {
    std::vector<DeviceInfo> out;
    if (!enumerator_) return out;
    for (EDataFlow flow : {eRender, eCapture}) {
        std::string defId, commsId;
        {
            ComPtr<IMMDevice> d;
            CoTaskPtr<wchar_t> id;
            if (SUCCEEDED(enumerator_->GetDefaultAudioEndpoint(flow, eConsole, &d)) && SUCCEEDED(d->GetId(id.put())))
                defId = toUtf8(id.get());
            ComPtr<IMMDevice> c;
            CoTaskPtr<wchar_t> cid;
            if (SUCCEEDED(enumerator_->GetDefaultAudioEndpoint(flow, eCommunications, &c)) &&
                SUCCEEDED(c->GetId(cid.put())))
                commsId = toUtf8(cid.get());
        }
        ComPtr<IMMDeviceCollection> coll;
        if (FAILED(enumerator_->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED, &coll)))
            continue;
        UINT count = 0;
        coll->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> d;
            if (FAILED(coll->Item(i, &d))) continue;
            DeviceInfo info;
            info.render = flow == eRender;
            CoTaskPtr<wchar_t> id;
            if (FAILED(d->GetId(id.put()))) continue;
            info.id = toUtf8(id.get());
            DWORD state = 0;
            d->GetState(&state);
            info.state = state == DEVICE_STATE_ACTIVE      ? "active"
                         : state == DEVICE_STATE_UNPLUGGED ? "unplugged"
                         : state == DEVICE_STATE_DISABLED  ? "disabled"
                                                           : "notpresent";
            ComPtr<IPropertyStore> props;
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props))) {
                PropVariant name, adapter;
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name))) info.name = toUtf8(name.str());
                if (SUCCEEDED(props->GetValue(PKEY_DeviceInterface_FriendlyName, &adapter)))
                    info.adapterName = toUtf8(adapter.str());
                PropVariant fmt;
                if (SUCCEEDED(props->GetValue(kPkeyDeviceFormat, &fmt)) && fmt.get().vt == VT_BLOB &&
                    fmt.get().blob.cbSize >= sizeof(WAVEFORMATEX)) {
                    // Device (hardware) format; the shared-mode mix rate matches it.
                    info.mixFormat = parseWaveFormat(reinterpret_cast<const WAVEFORMATEX*>(fmt.get().blob.pBlobData));
                    info.formatKnown = true;
                }
            }
            if (info.name.empty()) info.name = info.id;
            info.isDefault = info.id == defId;
            info.isDefaultComms = info.id == commsId;
            info.likelyVirtual = looksVirtual(info.name, info.adapterName);
            out.push_back(std::move(info));
        }
    }
    return out;
}

}  // namespace dbdeck::win
