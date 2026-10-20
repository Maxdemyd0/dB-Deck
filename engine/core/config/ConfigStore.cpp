#include "config/ConfigStore.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace dbdeck {

namespace fs = std::filesystem;

ConfigStore::ConfigStore(fs::path dir) : dir_(std::move(dir)) {}

static bool readFile(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool ConfigStore::writeFileAtomic(const fs::path& path, const std::string& data, std::string& error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = path.string() + ".tmp";
#ifdef _WIN32
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = "cannot create " + tmp.string() + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                    written == data.size() && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) {
        error = "write failed for " + tmp.string();
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "replace failed for " + path.string() + " (error " + std::to_string(GetLastError()) + ")";
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
#else
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << data;
        f.flush();
        if (!f) {
            error = "write failed";
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    return true;
#endif
}

fs::path ConfigStore::finalPath(const fs::path& p) {
#ifdef _WIN32
    HANDLE h = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring buf(1024, L'\0');
    DWORD n = GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()), VOLUME_NAME_DOS);
    if (n >= buf.size()) {
        buf.resize(n + 1);
        n = GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()), VOLUME_NAME_DOS);
    }
    CloseHandle(h);
    if (n == 0 || n >= buf.size()) return {};
    buf.resize(n);
    return buf;
#else
    std::error_code ec;
    fs::path r = fs::canonical(p, ec);
    return ec ? fs::path() : r;
#endif
}

bool ConfigStore::samePath(const fs::path& a, const fs::path& b) {
    auto norm = [](const fs::path& p) {
        std::wstring s = p.lexically_normal().wstring();
        if (s.rfind(LR"(\\?\UNC\)", 0) == 0) s = LR"(\\)" + s.substr(8);
        else if (s.rfind(LR"(\\?\)", 0) == 0) s = s.substr(4);
#ifdef _WIN32
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
        while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
#endif
        return s;
    };
    return norm(a) == norm(b);
}

// The folder is compared through the same resolver as the file, so short
// (8.3) names, junctions and subst drives do not count as redirection; only
// a file stored somewhere other than next to its folder does.
fs::path ConfigStore::redirection() const {
    const fs::path dir = resolve_(dir_);
    if (dir.empty()) return {};
    for (const fs::path& p : {mainPath(), backupPath()}) {
        std::error_code ec;
        if (!fs::exists(p, ec)) continue;
        const fs::path real = resolve_(p);
        if (!real.empty() && !samePath(real, dir / p.filename())) return real;
    }
    return {};
}

ConfigLoadResult ConfigStore::load() {
    ConfigLoadResult r;
    r.redirectedTo = redirection();
    if (!r.redirectedTo.empty()) {
        const std::u8string where = r.redirectedTo.u8string();
        r.warnings.push_back("Windows is redirecting the settings file to " + std::string(where.begin(), where.end()) +
                             " because dB-Deck was started from inside another app's package. These are not the "
                             "settings dB-Deck uses when started normally; restart it from the Start menu.");
    }
    auto tryParse = [&](const fs::path& p, AppConfig& out, std::string& err) -> bool {
        std::string text;
        if (!readFile(p, text)) {
            err = "not found";
            return false;
        }
        try {
            out = configFromJson(json::parse(text));
            return true;
        } catch (const json::exception& e) {
            err = std::string("invalid JSON: ") + e.what();
        } catch (const ConfigError& e) {
            err = e.what();
        }
        return false;
    };

    std::string err;
    const bool exists = fs::exists(mainPath());
    if (exists && tryParse(mainPath(), r.config, err)) {
        std::string werr;
        writeFileAtomic(backupPath(), toJson(r.config).dump(2), werr);
        return r;
    }
    if (exists) {
        const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        const fs::path keep = dir_ / ("config.corrupt-" + std::to_string(stamp) + ".json");
        std::error_code ec;
        fs::copy_file(mainPath(), keep, fs::copy_options::overwrite_existing, ec);
        r.warnings.push_back("Settings file could not be read (" + err + "). A copy was saved as " +
                             keep.filename().string() + ".");
    }
    std::string berr;
    if (fs::exists(backupPath()) && tryParse(backupPath(), r.config, berr)) {
        r.recoveredFromBackup = true;
        if (exists) r.warnings.push_back("Restored the last known good settings.");
        return r;
    }
    r.config = makeDefaultConfig();
    r.createdDefault = true;
    return r;
}

bool ConfigStore::save(const AppConfig& config, std::string& error) {
    return writeFileAtomic(mainPath(), toJson(config).dump(2), error);
}

}  // namespace dbdeck
