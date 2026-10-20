#include <filesystem>
#include <fstream>

#include "TestFramework.h"
#include "config/ConfigStore.h"
#include "graph/Model.h"

using namespace dbdeck;
namespace fs = std::filesystem;

static fs::path tempDir(const char* name) {
    fs::path p = fs::temp_directory_path() / "dbdeck-tests" / name;
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

static AppConfig sampleConfig() {
    AppConfig c = makeDefaultConfig();
    ProfileConfig& p = c.profiles[0];
    BusConfig stream;
    stream.id = newId();
    stream.name = "Stream";
    stream.deviceId = "{0.0.0.00000000}.{abc}";
    stream.fallbackDeviceIds = {"{x}"};
    p.buses.push_back(stream);
    GroupConfig g{newId(), "Games", 0.7f, false, true};
    p.groups.push_back(g);
    ChannelConfig ch = makeChannel(ChannelType::App, "Discord", p);
    ch.match.exeNames = {"discord.exe"};
    ch.volume = 0.42f;
    ch.groupId = g.id;
    ch.sends.push_back({stream.id, true, 0.65f, true});
    ch.effects.push_back({newId(), "compressor", true, {{"ratio", 4.0f}}});
    p.channels.push_back(ch);
    return c;
}

TEST("model: ids are unique v4 uuids") {
    std::string a = newId(), b = newId();
    CHECK(a != b);
    CHECK(a.size() == 36);
    CHECK(a[14] == '4');
}

TEST("model: default profile has exactly one Main bus") {
    AppConfig c = makeDefaultConfig();
    CHECK(c.profiles.size() == 1);
    CHECK(c.profiles[0].mainBus() != nullptr);
    CHECK(c.profiles[0].buses.size() == 1);
}

TEST("config: serialization round trip preserves everything") {
    AppConfig c = sampleConfig();
    json j = toJson(c);
    AppConfig back = configFromJson(json::parse(j.dump()));
    CHECK(toJson(back) == j);
    const ChannelConfig& ch = back.profiles[0].channels[0];
    CHECK(ch.match.exeNames[0] == "discord.exe");
    CHECK_NEAR(ch.volume, 0.42, 1e-6);
    CHECK(ch.sends[1].preFader);
    CHECK(ch.effects[0].params.at("ratio") == 4.0f);
}

TEST("config: invalid documents are rejected with a path") {
    json j = toJson(sampleConfig());
    j["profiles"][0]["channels"][0]["type"] = "bogus";
    try {
        configFromJson(j);
        CHECK(false);
    } catch (const ConfigError& e) {
        CHECK(std::string(e.what()).find("/profiles/0/channels/0/type") != std::string::npos);
    }
    json j2 = toJson(sampleConfig());
    j2["profiles"][0]["channels"][0]["volume"] = "loud";
    CHECK_THROWS(configFromJson(j2));
    json j3 = toJson(sampleConfig());
    j3["schemaVersion"] = 99;
    CHECK_THROWS(configFromJson(j3));
}

TEST("config: out-of-range values are clamped") {
    json j = toJson(sampleConfig());
    j["profiles"][0]["channels"][0]["volume"] = 7.5;
    AppConfig c = configFromJson(j);
    CHECK(c.profiles[0].channels[0].volume == 1.5f);  // fader maximum (+20 dB)
}

TEST("config: normalization repairs missing Main bus and dangling references") {
    ProfileConfig p = makeDefaultProfile("x");
    ChannelConfig ch = makeChannel(ChannelType::App, "a", p);
    ch.groupId = "missing";
    ch.sends.push_back({"ghost", true, 1.0f, false});
    p.channels.push_back(ch);
    p.buses.clear();
    auto notes = normalizeProfile(p);
    CHECK(!notes.empty());
    CHECK(p.mainBus() != nullptr);
    CHECK(p.channels[0].groupId.empty());
    for (auto& s : p.channels[0].sends) CHECK(s.busId != "ghost");
}

TEST("config store: atomic save then load") {
    ConfigStore store(tempDir("save"));
    AppConfig c = sampleConfig();
    std::string err;
    CHECK(store.save(c, err));
    CHECK(!fs::exists(store.mainPath().string() + ".tmp"));
    ConfigLoadResult r = store.load();
    CHECK(!r.createdDefault);
    CHECK(toJson(r.config) == toJson(c));
    CHECK(fs::exists(store.backupPath()));  // last-known-good refreshed
}

TEST("config store: corrupted file falls back to last known good") {
    ConfigStore store(tempDir("corrupt"));
    AppConfig c = sampleConfig();
    std::string err;
    CHECK(store.save(c, err));
    store.load();  // creates LKG
    {
        std::ofstream f(store.mainPath(), std::ios::trunc);
        f << "{ this is not json";
    }
    ConfigLoadResult r = store.load();
    CHECK(r.recoveredFromBackup);
    CHECK(!r.warnings.empty());
    CHECK(r.config.profiles[0].channels.size() == 1);
    bool keptCopy = false;
    for (auto& e : fs::directory_iterator(store.directory()))
        if (e.path().filename().string().rfind("config.corrupt-", 0) == 0) keptCopy = true;
    CHECK(keptCopy);
}

TEST("config store: missing file creates defaults") {
    ConfigStore store(tempDir("fresh"));
    ConfigLoadResult r = store.load();
    CHECK(r.createdDefault);
    CHECK(r.config.activeProfile() != nullptr);
}

TEST("config store: a normal folder is not reported as redirected") {
    ConfigStore store(tempDir("direct"));
    std::string err;
    CHECK(store.save(sampleConfig(), err));
    ConfigLoadResult r = store.load();
    CHECK(r.redirectedTo.empty());
    CHECK(r.warnings.empty());
}

TEST("config store: settings redirected by Windows (packaged-app AppData) are reported") {
    // A process started from inside an MSIX app sees that app's private
    // LocalCache\Roaming copy laid over %APPDATA%: the folder resolves to the
    // real place while the file resolves into the package.
    ConfigStore store(tempDir("redirected"));
    std::string err;
    CHECK(store.save(sampleConfig(), err));
    const fs::path virt = fs::path("C:/Users/x/AppData/Local/Packages/Some.App_123/LocalCache/Roaming/dB-Deck");
    store.setPathResolver([&](const fs::path& p) {
        if (p == store.directory()) return store.directory();
        return virt / p.filename();
    });
    ConfigLoadResult r = store.load();
    CHECK(ConfigStore::samePath(r.redirectedTo, virt / "config.json"));
    CHECK(!r.warnings.empty());
    CHECK(r.warnings[0].find("LocalCache") != std::string::npos);
    CHECK(!r.createdDefault);
    CHECK(r.config.profiles[0].channels.size() == 1);  // still loads what this process can see
}

#ifdef _WIN32
TEST("config store: path comparison ignores the long-path prefix, case and trailing separators") {
    CHECK(ConfigStore::samePath(R"(\\?\C:\Users\X\dB-Deck\config.json)", R"(c:\users\x\db-deck\config.json)"));
    CHECK(ConfigStore::samePath(R"(\\?\UNC\srv\share\a)", R"(\\srv\share\a\)"));
    CHECK(!ConfigStore::samePath(R"(C:\Users\x\AppData\Roaming\dB-Deck\config.json)",
                                 R"(C:\Users\x\AppData\Local\Packages\P\LocalCache\Roaming\dB-Deck\config.json)"));
}
#endif
