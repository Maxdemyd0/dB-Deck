#pragma once
// COM / Win32 helpers: RAII apartment init, HRESULT formatting, UTF-8/UTF-16
// conversion, PROPVARIANT and CoTaskMem ownership.

#include <windows.h>
#include <wrl/client.h>
#include <propidl.h>

#include <stdexcept>
#include <string>

namespace dbdeck::win {

using Microsoft::WRL::ComPtr;

std::string toUtf8(const std::wstring& w);
std::string toUtf8(const wchar_t* w);
std::wstring toWide(const std::string& s);
std::string hrToString(HRESULT hr);

struct HrError : std::runtime_error {
    HrError(HRESULT h, const std::string& what) : std::runtime_error(what + ": " + hrToString(h)), hr(h) {}
    HRESULT hr;
};

inline void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) throw HrError(hr, what);
}

// Initializes COM for the current thread (multithreaded apartment).
class ComApartment {
public:
    ComApartment() : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(hr_)) CoUninitialize();
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    bool ok() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT hr_;
};

class PropVariant {
public:
    PropVariant() { PropVariantInit(&pv_); }
    ~PropVariant() { PropVariantClear(&pv_); }
    PropVariant(const PropVariant&) = delete;
    PropVariant& operator=(const PropVariant&) = delete;
    PROPVARIANT* operator&() { return &pv_; }
    const PROPVARIANT& get() const { return pv_; }
    std::wstring str() const { return pv_.vt == VT_LPWSTR && pv_.pwszVal ? pv_.pwszVal : L""; }

private:
    PROPVARIANT pv_;
};

template <typename T>
class CoTaskPtr {
public:
    CoTaskPtr() = default;
    ~CoTaskPtr() { CoTaskMemFree(p_); }
    CoTaskPtr(const CoTaskPtr&) = delete;
    CoTaskPtr& operator=(const CoTaskPtr&) = delete;
    T** put() {
        CoTaskMemFree(p_);
        p_ = nullptr;
        return &p_;
    }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE h) : h_(h) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(UniqueHandle&& o) noexcept : h_(o.release()) {}
    UniqueHandle& operator=(UniqueHandle&& o) noexcept {
        if (this != &o) reset(o.release());
        return *this;
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    HANDLE get() const { return h_; }
    HANDLE release() {
        HANDLE h = h_;
        h_ = nullptr;
        return h;
    }
    void reset(HANDLE h = nullptr) {
        if (h_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = h;
    }
    explicit operator bool() const { return h_ && h_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE h_ = nullptr;
};

// Sets flush-to-zero / denormals-are-zero and registers the thread with MMCSS
// ("Pro Audio"). Returns the MMCSS handle (may be null).
HANDLE enterRealtimeAudioThread();
void leaveRealtimeAudioThread(HANDLE mmcss);

}  // namespace dbdeck::win
