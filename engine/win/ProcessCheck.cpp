#include "ProcessCheck.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>
#include <set>

#include "ComUtil.h"

namespace dbdeck::win {

std::vector<ProcessEntry> findProcessesByPrefix(const std::wstring& prefix) {
    std::vector<ProcessEntry> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        std::wstring name = pe.szExeFile;
        std::wstring lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (lower.rfind(prefix, 0) == 0) out.push_back({pe.th32ProcessID, toUtf8(name)});
    }
    CloseHandle(snap);
    return out;
}

namespace {
struct CloseContext {
    std::set<DWORD> pids;
    bool visibleOnly = true;
    int posted = 0;
};

BOOL CALLBACK closeWindow(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<CloseContext*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!ctx->pids.count(pid) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    if (ctx->visibleOnly && !IsWindowVisible(hwnd)) return TRUE;
    if (PostMessageW(hwnd, WM_CLOSE, 0, 0)) ++ctx->posted;
    return TRUE;
}
}  // namespace

int requestClose(const std::vector<ProcessEntry>& processes) {
    CloseContext ctx;
    for (auto& p : processes) ctx.pids.insert(p.pid);
    if (ctx.pids.empty()) return 0;
    EnumWindows(closeWindow, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.posted == 0) {
        // Minimized to the tray: its main window is hidden.
        ctx.visibleOnly = false;
        EnumWindows(closeWindow, reinterpret_cast<LPARAM>(&ctx));
    }
    return ctx.posted;
}

}  // namespace dbdeck::win
