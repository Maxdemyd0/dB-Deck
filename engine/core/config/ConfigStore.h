#pragma once
// Persistent configuration: versioned JSON written atomically (temp file +
// flush + replace), with a last-known-good copy refreshed after every
// successful load. A malformed file is preserved for inspection and the
// last-known-good copy (or defaults) is used instead.

#include <filesystem>
#include <string>
#include <vector>

#include "graph/Model.h"

namespace dbdeck {

struct ConfigLoadResult {
    AppConfig config;
    std::vector<std::string> warnings;
    bool recoveredFromBackup = false;
    bool createdDefault = false;
};

class ConfigStore {
public:
    explicit ConfigStore(std::filesystem::path dir);

    ConfigLoadResult load();
    bool save(const AppConfig& config, std::string& error);

    const std::filesystem::path& directory() const { return dir_; }
    std::filesystem::path mainPath() const { return dir_ / "config.json"; }
    std::filesystem::path backupPath() const { return dir_ / "config.lkg.json"; }

    // Exposed for tests.
    static bool writeFileAtomic(const std::filesystem::path& path, const std::string& data, std::string& error);

private:
    std::filesystem::path dir_;
};

}  // namespace dbdeck
