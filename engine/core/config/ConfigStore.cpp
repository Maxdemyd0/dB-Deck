#include "config/ConfigStore.h"

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

ConfigLoadResult ConfigStore::load() {
    ConfigLoadResult r;
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
