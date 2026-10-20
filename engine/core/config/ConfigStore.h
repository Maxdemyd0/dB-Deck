#pragma once
// Persistent configuration: versioned JSON written atomically (temp file +
// flush + replace), with a last-known-good copy refreshed after every
// successful load. A malformed file is preserved for inspection and the
// last-known-good copy (or defaults) is used instead.
//
// Windows can silently redirect the files to another place: a process
// started from a packaged (MSIX) app, such as a terminal inside one, has new
// %APPDATA% files written to that app's private
// %LOCALAPPDATA%\Packages\<app>\LocalCache\Roaming and sees them laid over the
// real folder. Such a process reads and writes settings the normally started
// app never sees, so the redirection is reported as a load warning.

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "graph/Model.h"

namespace dbdeck {

struct ConfigLoadResult {
    AppConfig config;
    std::vector<std::string> warnings;
    bool recoveredFromBackup = false;
    bool createdDefault = false;
    // Where Windows really keeps the settings file when it is redirected
    // away from directory(); empty when it is not.
    std::filesystem::path redirectedTo;
};

class ConfigStore {
public:
    explicit ConfigStore(std::filesystem::path dir);

    ConfigLoadResult load();
    bool save(const AppConfig& config, std::string& error);

    const std::filesystem::path& directory() const { return dir_; }
    std::filesystem::path mainPath() const { return dir_ / "config.json"; }
    std::filesystem::path backupPath() const { return dir_ / "config.lkg.json"; }

    // Maps an existing path to where the OS actually stores it (empty when
    // unknown). Defaults to finalPath(); replaceable for tests.
    using PathResolver = std::function<std::filesystem::path(const std::filesystem::path&)>;
    void setPathResolver(PathResolver resolver) { resolve_ = std::move(resolver); }

    // Exposed for tests.
    static bool writeFileAtomic(const std::filesystem::path& path, const std::string& data, std::string& error);
    static std::filesystem::path finalPath(const std::filesystem::path& p);
    static bool samePath(const std::filesystem::path& a, const std::filesystem::path& b);

private:
    std::filesystem::path redirection() const;

    std::filesystem::path dir_;
    PathResolver resolve_ = &ConfigStore::finalPath;
};

}  // namespace dbdeck
