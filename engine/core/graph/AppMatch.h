#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

#include "graph/Model.h"

namespace dbdeck {

struct ProcessIdentity {
    uint32_t pid = 0;
    std::string exeName;  // lower-case file name
    std::string exePath;  // full path (may be empty if access was denied)
};

inline std::string toLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// An App channel matches every process whose executable name is in the rule
// (so multi-process apps such as Discord or browsers bind as one channel),
// optionally narrowed by a path substring.
inline bool matchesRule(const AppMatchRule& rule, const ProcessIdentity& proc) {
    if (rule.exeNames.empty() || proc.exeName.empty()) return false;
    const std::string exe = toLowerAscii(proc.exeName);
    const bool nameOk = std::any_of(rule.exeNames.begin(), rule.exeNames.end(),
                                    [&](const std::string& n) { return toLowerAscii(n) == exe; });
    if (!nameOk) return false;
    if (rule.pathContains.empty()) return true;
    return toLowerAscii(proc.exePath).find(toLowerAscii(rule.pathContains)) != std::string::npos;
}

inline AppMatchRule ruleForExe(const std::string& exeName) {
    AppMatchRule r;
    r.exeNames.push_back(toLowerAscii(exeName));
    return r;
}

}  // namespace dbdeck
