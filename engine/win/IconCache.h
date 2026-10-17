#pragma once
// Application icons extracted from executables and encoded as PNG data URLs
// (WIC), cached by path. Control thread only.

#include <map>
#include <string>

namespace dbdeck::win {

class IconCache {
public:
    // Returns "data:image/png;base64,..." or "" if unavailable.
    const std::string& iconFor(const std::wstring& exePath);

private:
    std::map<std::wstring, std::string> cache_;
};

}  // namespace dbdeck::win
