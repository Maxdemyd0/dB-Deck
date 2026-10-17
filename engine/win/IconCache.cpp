#include "IconCache.h"

#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>

#include <vector>

#include "ComUtil.h"

namespace dbdeck::win {

namespace {

std::string base64(const std::vector<unsigned char>& data) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const unsigned v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
    }
    if (i < data.size()) {
        unsigned v = data[i] << 16;
        if (i + 1 < data.size()) v |= data[i + 1] << 8;
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += i + 1 < data.size() ? tbl[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string encodeIcon(HICON icon) {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return {};
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmapFromHICON(icon, &bitmap))) return {};
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return {};
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) return {};
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return {};
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) return {};
    UINT w = 0, h = 0;
    bitmap->GetSize(&w, &h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetSize(w, h)) || FAILED(frame->SetPixelFormat(&fmt)) ||
        FAILED(frame->WriteSource(bitmap.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        return {};
    HGLOBAL mem = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.Get(), &mem))) return {};
    STATSTG st{};
    stream->Stat(&st, STATFLAG_NONAME);
    const size_t size = static_cast<size_t>(st.cbSize.QuadPart);
    std::vector<unsigned char> bytes(size);
    if (void* p = GlobalLock(mem)) {
        memcpy(bytes.data(), p, size);
        GlobalUnlock(mem);
    } else {
        return {};
    }
    return "data:image/png;base64," + base64(bytes);
}

}  // namespace

const std::string& IconCache::iconFor(const std::wstring& exePath) {
    auto it = cache_.find(exePath);
    if (it != cache_.end()) return it->second;
    std::string encoded;
    if (!exePath.empty()) {
        HICON large = nullptr;
        if (ExtractIconExW(exePath.c_str(), 0, &large, nullptr, 1) > 0 && large) {
            encoded = encodeIcon(large);
            DestroyIcon(large);
        }
        if (encoded.empty()) {
            SHFILEINFOW info{};
            if (SHGetFileInfoW(exePath.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) && info.hIcon) {
                encoded = encodeIcon(info.hIcon);
                DestroyIcon(info.hIcon);
            }
        }
    }
    return cache_.emplace(exePath, std::move(encoded)).first->second;
}

}  // namespace dbdeck::win
