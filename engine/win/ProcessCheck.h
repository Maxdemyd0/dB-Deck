#pragma once
// Finds other audio mixers that may hold devices (Voicemeeter), and asks them
// to close. Closing is graceful (WM_CLOSE to the app's windows, as if the user
// clicked X) and only happens when the user asks for it.

#include <cstdint>
#include <string>
#include <vector>

namespace dbdeck::win {

struct ProcessEntry {
    uint32_t pid = 0;
    std::string exeName;  // e.g. "voicemeeter8x64.exe"
};

// Running processes whose executable name starts with `prefix` (case-insensitive).
std::vector<ProcessEntry> findProcessesByPrefix(const std::wstring& prefix);

// Running Voicemeeter instances (Standard, Banana, Potato; any bitness).
inline std::vector<ProcessEntry> findVoicemeeter() { return findProcessesByPrefix(L"voicemeeter"); }

// Posts WM_CLOSE to the top-level windows of each process. Returns the number
// of windows asked to close. Never terminates a process.
int requestClose(const std::vector<ProcessEntry>& processes);

}  // namespace dbdeck::win
