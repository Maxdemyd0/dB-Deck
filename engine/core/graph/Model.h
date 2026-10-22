#pragma once
// Declarative mixer model: what the user configured. The engine compiles this
// into a real-time graph (graph/Runtime.h). Everything here is owned and
// mutated by the control thread only.

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace dbdeck {

using json = nlohmann::json;

enum class ChannelType {
    App,               // Windows audio session(s) of an application
    HardwareInput,     // WASAPI capture endpoint (microphone, line in, virtual cable output)
    EndpointLoopback,  // loopback capture of a playback endpoint (e.g. a virtual cable input)
    TestTone,          // generated sine, for verification
};

const char* channelTypeName(ChannelType t);
std::optional<ChannelType> channelTypeFromString(const std::string& s);

// Rules for binding an App channel to Windows audio sessions.
struct AppMatchRule {
    std::vector<std::string> exeNames;  // lower-case file names, e.g. "discord.exe"
    std::string pathContains;           // optional case-insensitive substring of the full path

    bool empty() const { return exeNames.empty(); }
};

struct SendConfig {
    std::string busId;
    bool enabled = true;
    float gain = 1.0f;       // linear send level (0..2)
    bool preFader = false;   // pre-fader: send ignores channel fader/mute
};

struct EffectConfig {
    std::string id;
    std::string type;  // "gain" | "eq" | "gate" | "compressor" | "limiter"
    bool enabled = true;
    std::map<std::string, float> params;
};

struct ChannelConfig {
    std::string id;
    std::string name;
    ChannelType type = ChannelType::App;

    AppMatchRule match;          // App
    bool appCapture = false;     // App: capture PCM via process loopback (managed audio)
    std::string deviceId;        // HardwareInput / EndpointLoopback: stable endpoint ID
    std::string deviceName;      // last known friendly name (display only)
    float toneHz = 440.0f;       // TestTone

    float volume = 1.0f;   // fader position 0..1
    float gainDb = 0.0f;   // input trim (managed audio)
    float balance = 0.0f;  // -1..1 (managed audio)
    bool mute = false;

    std::string groupId;
    std::vector<SendConfig> sends;
    std::vector<EffectConfig> effects;
    std::string reconnect = "auto";  // "auto" | "manual"
    std::string color;               // strip color "#rrggbb" ("" = none; display only)

    const SendConfig* findSend(const std::string& busId) const;
    SendConfig* findSend(const std::string& busId);
};

struct GroupConfig {
    std::string id;
    std::string name;
    float volume = 1.0f;
    bool mute = false;
    bool collapsed = false;
};

struct BusConfig {
    std::string id;
    std::string name;
    bool isMain = false;
    float volume = 1.0f;
    bool mute = false;
    std::string deviceId;                        // destination playback endpoint ("" = none)
    std::string deviceName;                      // last known friendly name (display only)
    std::vector<std::string> fallbackDeviceIds;  // explicit, user-authorised fallbacks in order
    std::vector<SendConfig> busSends;            // this bus feeds other buses
    std::vector<EffectConfig> effects;
    std::string color;  // strip color "#rrggbb" ("" = none; display only)
};

struct SceneChannelState {
    float volume = 1.0f;
    bool mute = false;
    std::map<std::string, float> sendGains;
    std::map<std::string, bool> sendEnabled;
};

struct SceneConfig {
    std::string id;
    std::string name;
    std::map<std::string, SceneChannelState> channels;
    std::map<std::string, std::pair<float, bool>> groups;  // volume, mute
    std::map<std::string, std::pair<float, bool>> buses;   // volume, mute
};

struct ProfileConfig {
    std::string id;
    std::string name;
    std::vector<ChannelConfig> channels;  // display order
    std::vector<GroupConfig> groups;
    std::vector<BusConfig> buses;
    std::vector<SceneConfig> scenes;
    json layout = json::object();  // UI-owned layout data (opaque to the engine)

    ChannelConfig* findChannel(const std::string& id);
    const ChannelConfig* findChannel(const std::string& id) const;
    GroupConfig* findGroup(const std::string& id);
    const GroupConfig* findGroup(const std::string& id) const;
    BusConfig* findBus(const std::string& id);
    const BusConfig* findBus(const std::string& id) const;
    BusConfig* mainBus();
    const BusConfig* mainBus() const;
    SceneConfig* findScene(const std::string& id);
};

struct EngineSettings {
    std::string bufferMode = "normal";  // "low" | "normal" | "safe"
    int meterRateHz = 30;
    bool showSystemSounds = false;
};

struct AppConfig {
    static constexpr int kSchemaVersion = 1;
    int schemaVersion = kSchemaVersion;
    std::string activeProfileId;
    EngineSettings settings;
    std::vector<ProfileConfig> profiles;

    ProfileConfig* findProfile(const std::string& id);
    ProfileConfig* activeProfile();
};

// True for "" or a "#rrggbb" color.
bool isValidColor(const std::string& s);

// Builders for defaults.
std::string newId();
BusConfig makeMainBus();
ProfileConfig makeDefaultProfile(const std::string& name);
AppConfig makeDefaultConfig();
// New channels always get an enabled Main send at unity (product requirement).
ChannelConfig makeChannel(ChannelType type, const std::string& name, const ProfileConfig& profile);

// Serialization. Parsing validates types and ranges and throws ConfigError
// with a JSON-pointer-like path on failure.
struct ConfigError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

json toJson(const ChannelConfig&);
json toJson(const GroupConfig&);
json toJson(const BusConfig&);
json toJson(const EffectConfig&);
json toJson(const SceneConfig&);
json toJson(const ProfileConfig&);
json toJson(const AppConfig&);

ChannelConfig channelFromJson(const json&, const std::string& path);
EffectConfig effectFromJson(const json&, const std::string& path);
ProfileConfig profileFromJson(const json&, const std::string& path);
AppConfig configFromJson(const json&);

// Repairs structural invariants after loading (exactly one Main bus, sends
// to missing buses dropped, unknown groups cleared). Returns repair notes.
std::vector<std::string> normalizeProfile(ProfileConfig& p);

}  // namespace dbdeck
