#include "AppOutputRouting.h"

#include <windows.h>
#include <roapi.h>
#include <winstring.h>

#include "ComUtil.h"

#pragma comment(lib, "runtimeobject.lib")

namespace dbdeck::win {

namespace {

// IAudioPolicyConfigFactory (undocumented). Layout after IInspectable:
// 19 methods we do not use, then SetPersistedDefaultAudioEndpoint and
// GetPersistedDefaultAudioEndpoint. Indices include IUnknown (3) and
// IInspectable (3). Same layout as used by EarTrumpet (MIT).
constexpr int kSetPersistedIndex = 6 + 19;
constexpr int kGetPersistedIndex = 6 + 20;

// Windows 10 21H2+ / Windows 11, and the older Windows 10 variant.
constexpr GUID kIidFactoryNew = {0xab3d4648, 0xe242, 0x459f, {0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24}};
constexpr GUID kIidFactoryOld = {0x2a59116d, 0x6c4f, 0x45e0, {0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58}};

constexpr wchar_t kMmdevPrefix[] = L"\\\\?\\SWD#MMDEVAPI#";
constexpr wchar_t kRenderSuffix[] = L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";  // DEVINTERFACE_AUDIO_RENDER

using SetFn = HRESULT(STDMETHODCALLTYPE*)(void* self, UINT32 pid, int flow, int role, HSTRING id);
using GetFn = HRESULT(STDMETHODCALLTYPE*)(void* self, UINT32 pid, int flow, int role, HSTRING* id);

template <typename Fn>
Fn method(void* obj, int index) {
    void** vtbl = *static_cast<void***>(obj);
    return reinterpret_cast<Fn>(vtbl[index]);
}

class HString {
public:
    explicit HString(const std::wstring& s) { WindowsCreateString(s.c_str(), static_cast<UINT32>(s.size()), &h_); }
    ~HString() {
        if (h_) WindowsDeleteString(h_);
    }
    HSTRING get() const { return h_; }

private:
    HSTRING h_ = nullptr;
};

}  // namespace

AppOutputRouting::AppOutputRouting() {
    const std::wstring cls = L"Windows.Media.Internal.AudioPolicyConfig";
    HString name(cls);
    void* f = nullptr;
    HRESULT hr = RoGetActivationFactory(name.get(), kIidFactoryNew, &f);
    if (FAILED(hr)) {
        hr = RoGetActivationFactory(name.get(), kIidFactoryOld, &f);
        newLayout_ = false;
    }
    if (SUCCEEDED(hr) && f) factory_ = f;
    else reason_ = "Per-app output selection is not available on this version of Windows (" + hrToString(hr) + ")";
}

AppOutputRouting::~AppOutputRouting() {
    if (factory_) static_cast<IUnknown*>(factory_)->Release();
}

bool AppOutputRouting::setOutput(unsigned pid, const std::string& deviceId, std::string& error) {
    if (!factory_) {
        error = reason_;
        return false;
    }
    auto set = method<SetFn>(factory_, kSetPersistedIndex);
    const std::wstring full = deviceId.empty() ? std::wstring() : kMmdevPrefix + toWide(deviceId) + kRenderSuffix;
    HString h(full);
    for (int role : {0 /* eConsole */, 1 /* eMultimedia */}) {
        const HRESULT hr = set(factory_, pid, 0 /* eRender */, role, deviceId.empty() ? nullptr : h.get());
        if (FAILED(hr)) {
            error = "Windows refused the output change: " + hrToString(hr);
            return false;
        }
    }
    return true;
}

std::string AppOutputRouting::getOutput(unsigned pid) {
    if (!factory_) return {};
    auto get = method<GetFn>(factory_, kGetPersistedIndex);
    HSTRING h = nullptr;
    if (FAILED(get(factory_, pid, 0 /* eRender */, 1 /* eMultimedia */, &h)) || !h) return {};
    UINT32 len = 0;
    const wchar_t* raw = WindowsGetStringRawBuffer(h, &len);
    std::wstring s(raw, len);
    WindowsDeleteString(h);
    // "\\?\SWD#MMDEVAPI#{0.0.0.00000000}.{guid}#{interface}" -> "{0.0.0.00000000}.{guid}"
    const std::wstring prefix = kMmdevPrefix;
    if (s.rfind(prefix, 0) == 0) s = s.substr(prefix.size());
    if (const size_t hash = s.find(L'#'); hash != std::wstring::npos) s = s.substr(0, hash);
    return toUtf8(s);
}

}  // namespace dbdeck::win
