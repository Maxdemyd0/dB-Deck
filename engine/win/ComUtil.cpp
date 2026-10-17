#include "ComUtil.h"

#include <avrt.h>
#include <xmmintrin.h>
#include <pmmintrin.h>

#include <cstdio>

namespace dbdeck::win {

std::string toUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(len > 0 ? len - 1 : 0), '\0');
    if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}

std::string toUtf8(const std::wstring& w) { return toUtf8(w.c_str()); }

std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(len > 0 ? len - 1 : 0), L'\0');
    if (len > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
}

std::string hrToString(HRESULT hr) {
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
    // Common audio errors by name: FormatMessage does not know most of them.
    switch (hr) {
        case static_cast<HRESULT>(0x88890004): return std::string(code) + " AUDCLNT_E_DEVICE_INVALIDATED";
        case static_cast<HRESULT>(0x8889000A): return std::string(code) + " AUDCLNT_E_DEVICE_IN_USE";
        case static_cast<HRESULT>(0x88890008): return std::string(code) + " AUDCLNT_E_UNSUPPORTED_FORMAT";
        case static_cast<HRESULT>(0x88890001): return std::string(code) + " AUDCLNT_E_NOT_INITIALIZED";
        case static_cast<HRESULT>(0x8889000F): return std::string(code) + " AUDCLNT_E_ENDPOINT_CREATE_FAILED";
        case static_cast<HRESULT>(0x88890026): return std::string(code) + " AUDCLNT_E_RESOURCES_INVALIDATED";
        case static_cast<HRESULT>(0x80070490): return std::string(code) + " E_NOTFOUND";
        default: break;
    }
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::string text = msg ? toUtf8(msg) : std::string();
    if (msg) LocalFree(msg);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text.empty() ? std::string(code) : std::string(code) + " " + text;
}

HANDLE enterRealtimeAudioThread() {
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
    DWORD task = 0;
    HANDLE h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    if (!h) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    return h;
}

void leaveRealtimeAudioThread(HANDLE mmcss) {
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

}  // namespace dbdeck::win
