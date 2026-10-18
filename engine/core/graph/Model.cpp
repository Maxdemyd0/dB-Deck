#include "graph/Model.h"

#include "audio/Gain.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <random>
#include <set>

namespace dbdeck {

// ---------------------------------------------------------------------------
// Basics

const char* channelTypeName(ChannelType t) {
    switch (t) {
        case ChannelType::App: return "app";
        case ChannelType::HardwareInput: return "hardwareInput";
        case ChannelType::EndpointLoopback: return "endpointLoopback";
        case ChannelType::TestTone: return "testTone";
    }
    return "app";
}

std::optional<ChannelType> channelTypeFromString(const std::string& s) {
    for (auto t : {ChannelType::App, ChannelType::HardwareInput, ChannelType::EndpointLoopback,
                   ChannelType::TestTone})
        if (s == channelTypeName(t)) return t;
    return std::nullopt;
}

std::string newId() {
    thread_local std::mt19937_64 rng{std::random_device{}() ^ (static_cast<uint64_t>(std::random_device{}()) << 32)};
    uint64_t a = rng(), b = rng();
    a = (a & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;  // version 4
    b = (b & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;  // variant 1
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx", static_cast<uint32_t>(a >> 32),
                  static_cast<uint32_t>((a >> 16) & 0xFFFF), static_cast<uint32_t>(a & 0xFFFF),
                  static_cast<uint32_t>(b >> 48), static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFull));
    return buf;
}

const SendConfig* ChannelConfig::findSend(const std::string& busId) const {
    for (auto& s : sends)
        if (s.busId == busId) return &s;
    return nullptr;
}
SendConfig* ChannelConfig::findSend(const std::string& busId) {
    for (auto& s : sends)
        if (s.busId == busId) return &s;
    return nullptr;
}

template <typename T>
static T* findById(std::vector<T>& v, const std::string& id) {
    for (auto& x : v)
        if (x.id == id) return &x;
    return nullptr;
}

ChannelConfig* ProfileConfig::findChannel(const std::string& key) { return findById(channels, key); }
const ChannelConfig* ProfileConfig::findChannel(const std::string& key) const {
    return const_cast<ProfileConfig*>(this)->findChannel(key);
}
GroupConfig* ProfileConfig::findGroup(const std::string& key) { return findById(groups, key); }
const GroupConfig* ProfileConfig::findGroup(const std::string& key) const {
    return const_cast<ProfileConfig*>(this)->findGroup(key);
}
BusConfig* ProfileConfig::findBus(const std::string& key) { return findById(buses, key); }
const BusConfig* ProfileConfig::findBus(const std::string& key) const {
    return const_cast<ProfileConfig*>(this)->findBus(key);
}
BusConfig* ProfileConfig::mainBus() {
    for (auto& b : buses)
        if (b.isMain) return &b;
    return nullptr;
}
const BusConfig* ProfileConfig::mainBus() const { return const_cast<ProfileConfig*>(this)->mainBus(); }
SceneConfig* ProfileConfig::findScene(const std::string& key) { return findById(scenes, key); }

ProfileConfig* AppConfig::findProfile(const std::string& key) { return findById(profiles, key); }
ProfileConfig* AppConfig::activeProfile() {
    if (auto* p = findProfile(activeProfileId)) return p;
    return profiles.empty() ? nullptr : &profiles.front();
}

BusConfig makeMainBus() {
    BusConfig b;
    b.id = "main";
    b.name = "Main";
    b.isMain = true;
    return b;
}

ProfileConfig makeDefaultProfile(const std::string& name) {
    ProfileConfig p;
    p.id = newId();
    p.name = name;
    p.buses.push_back(makeMainBus());
    return p;
}

AppConfig makeDefaultConfig() {
    AppConfig c;
    c.profiles.push_back(makeDefaultProfile("Default"));
    c.activeProfileId = c.profiles.front().id;
    return c;
}

ChannelConfig makeChannel(ChannelType type, const std::string& name, const ProfileConfig& profile) {
    ChannelConfig c;
    c.id = newId();
    c.name = name;
    c.type = type;
    if (const BusConfig* main = profile.mainBus()) c.sends.push_back(SendConfig{main->id, true, 1.0f, false});
    return c;
}

// ---------------------------------------------------------------------------
// Serialization

json toJson(const EffectConfig& e) {
    json params = json::object();
    for (auto& [k, v] : e.params) params[k] = v;
    return {{"id", e.id}, {"type", e.type}, {"enabled", e.enabled}, {"params", params}};
}

static json sendsToJson(const std::vector<SendConfig>& sends) {
    json arr = json::array();
    for (auto& s : sends)
        arr.push_back({{"busId", s.busId}, {"enabled", s.enabled}, {"gain", s.gain}, {"preFader", s.preFader}});
    return arr;
}

static json effectsToJson(const std::vector<EffectConfig>& effects) {
    json arr = json::array();
    for (auto& e : effects) arr.push_back(toJson(e));
    return arr;
}

json toJson(const ChannelConfig& c) {
    return {{"id", c.id},
            {"name", c.name},
            {"type", channelTypeName(c.type)},
            {"match", {{"exeNames", c.match.exeNames}, {"pathContains", c.match.pathContains}}},
            {"appCapture", c.appCapture},
            {"deviceId", c.deviceId},
            {"deviceName", c.deviceName},
            {"toneHz", c.toneHz},
            {"volume", c.volume},
            {"gainDb", c.gainDb},
            {"balance", c.balance},
            {"mute", c.mute},
            {"groupId", c.groupId},
            {"sends", sendsToJson(c.sends)},
            {"effects", effectsToJson(c.effects)},
            {"reconnect", c.reconnect}};
}

json toJson(const GroupConfig& g) {
    return {{"id", g.id}, {"name", g.name}, {"volume", g.volume}, {"mute", g.mute}, {"collapsed", g.collapsed}};
}

json toJson(const BusConfig& b) {
    return {{"id", b.id},
            {"name", b.name},
            {"isMain", b.isMain},
            {"volume", b.volume},
            {"mute", b.mute},
            {"deviceId", b.deviceId},
            {"deviceName", b.deviceName},
            {"fallbackDeviceIds", b.fallbackDeviceIds},
            {"busSends", sendsToJson(b.busSends)},
            {"effects", effectsToJson(b.effects)}};
}

json toJson(const SceneConfig& s) {
    json ch = json::object();
    for (auto& [id, st] : s.channels) {
        json gains = json::object(), enabled = json::object();
        for (auto& [b, g] : st.sendGains) gains[b] = g;
        for (auto& [b, e] : st.sendEnabled) enabled[b] = e;
        ch[id] = {{"volume", st.volume}, {"mute", st.mute}, {"sendGains", gains}, {"sendEnabled", enabled}};
    }
    json groups = json::object(), buses = json::object();
    for (auto& [id, v] : s.groups) groups[id] = {{"volume", v.first}, {"mute", v.second}};
    for (auto& [id, v] : s.buses) buses[id] = {{"volume", v.first}, {"mute", v.second}};
    return {{"id", s.id}, {"name", s.name}, {"channels", ch}, {"groups", groups}, {"buses", buses}};
}

json toJson(const ProfileConfig& p) {
    json channels = json::array(), groups = json::array(), buses = json::array(), scenes = json::array();
    for (auto& c : p.channels) channels.push_back(toJson(c));
    for (auto& g : p.groups) groups.push_back(toJson(g));
    for (auto& b : p.buses) buses.push_back(toJson(b));
    for (auto& s : p.scenes) scenes.push_back(toJson(s));
    return {{"id", p.id},         {"name", p.name},     {"channels", channels}, {"groups", groups},
            {"buses", buses},     {"scenes", scenes},   {"layout", p.layout}};
}

json toJson(const AppConfig& c) {
    json profiles = json::array();
    for (auto& p : c.profiles) profiles.push_back(toJson(p));
    return {{"schemaVersion", c.schemaVersion},
            {"activeProfileId", c.activeProfileId},
            {"settings",
             {{"bufferMode", c.settings.bufferMode},
              {"meterRateHz", c.settings.meterRateHz},
              {"showSystemSounds", c.settings.showSystemSounds}}},
            {"profiles", profiles}};
}

// --- parsing helpers --------------------------------------------------------

namespace {

[[noreturn]] void fail(const std::string& path, const std::string& msg) { throw ConfigError(path + ": " + msg); }

const json& require(const json& j, const char* key, const std::string& path) {
    if (!j.is_object()) fail(path, "expected object");
    auto it = j.find(key);
    if (it == j.end()) fail(path + "/" + key, "missing");
    return *it;
}

std::string getString(const json& j, const char* key, const std::string& path, const std::string& def,
                      bool required = false) {
    if (!j.is_object()) fail(path, "expected object");
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        if (required) fail(path + "/" + key, "missing");
        return def;
    }
    if (!it->is_string()) fail(path + "/" + key, "expected string");
    return it->get<std::string>();
}

float getFloat(const json& j, const char* key, const std::string& path, float def, float lo, float hi) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    if (!it->is_number()) fail(path + "/" + key, "expected number");
    const double v = it->get<double>();
    if (!std::isfinite(v)) fail(path + "/" + key, "not finite");
    return static_cast<float>(std::clamp(v, static_cast<double>(lo), static_cast<double>(hi)));
}

bool getBool(const json& j, const char* key, const std::string& path, bool def) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    if (!it->is_boolean()) fail(path + "/" + key, "expected boolean");
    return it->get<bool>();
}

const json* getArray(const json& j, const char* key, const std::string& path) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return nullptr;
    if (!it->is_array()) fail(path + "/" + key, "expected array");
    return &*it;
}

std::vector<SendConfig> sendsFromJson(const json& j, const char* key, const std::string& path) {
    std::vector<SendConfig> out;
    if (const json* arr = getArray(j, key, path)) {
        for (size_t i = 0; i < arr->size(); ++i) {
            const std::string p = path + "/" + key + "/" + std::to_string(i);
            const json& s = (*arr)[i];
            SendConfig sc;
            sc.busId = getString(s, "busId", p, "", true);
            sc.enabled = getBool(s, "enabled", p, true);
            sc.gain = getFloat(s, "gain", p, 1.0f, 0.0f, 2.0f);
            sc.preFader = getBool(s, "preFader", p, false);
            out.push_back(std::move(sc));
        }
    }
    return out;
}

std::vector<EffectConfig> effectsFromJson(const json& j, const std::string& path) {
    std::vector<EffectConfig> out;
    if (const json* arr = getArray(j, "effects", path))
        for (size_t i = 0; i < arr->size(); ++i)
            out.push_back(effectFromJson((*arr)[i], path + "/effects/" + std::to_string(i)));
    return out;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

EffectConfig effectFromJson(const json& j, const std::string& path) {
    EffectConfig e;
    e.id = getString(j, "id", path, "");
    if (e.id.empty()) e.id = newId();
    e.type = getString(j, "type", path, "", true);
    static const std::set<std::string> known = {"gain", "eq", "gate", "compressor", "limiter"};
    if (!known.count(e.type)) fail(path + "/type", "unknown effect type '" + e.type + "'");
    e.enabled = getBool(j, "enabled", path, true);
    if (auto it = j.find("params"); it != j.end() && !it->is_null()) {
        if (!it->is_object()) fail(path + "/params", "expected object");
        for (auto& [k, v] : it->items()) {
            if (!v.is_number()) fail(path + "/params/" + k, "expected number");
            const double d = v.get<double>();
            if (!std::isfinite(d)) fail(path + "/params/" + k, "not finite");
            e.params[k] = static_cast<float>(d);
        }
    }
    return e;
}

ChannelConfig channelFromJson(const json& j, const std::string& path) {
    ChannelConfig c;
    c.id = getString(j, "id", path, "", true);
    c.name = getString(j, "name", path, "Channel");
    const std::string type = getString(j, "type", path, "", true);
    auto t = channelTypeFromString(type);
    if (!t) fail(path + "/type", "unknown channel type '" + type + "'");
    c.type = *t;
    if (auto it = j.find("match"); it != j.end() && it->is_object()) {
        if (const json* names = getArray(*it, "exeNames", path + "/match"))
            for (auto& n : *names) {
                if (!n.is_string()) fail(path + "/match/exeNames", "expected strings");
                c.match.exeNames.push_back(lower(n.get<std::string>()));
            }
        c.match.pathContains = getString(*it, "pathContains", path + "/match", "");
    }
    c.appCapture = getBool(j, "appCapture", path, false);
    c.deviceId = getString(j, "deviceId", path, "");
    c.deviceName = getString(j, "deviceName", path, "");
    c.toneHz = getFloat(j, "toneHz", path, 440.0f, 20.0f, 20000.0f);
    c.volume = getFloat(j, "volume", path, 1.0f, 0.0f, kFaderMax);
    c.gainDb = getFloat(j, "gainDb", path, 0.0f, -60.0f, 24.0f);
    c.balance = getFloat(j, "balance", path, 0.0f, -1.0f, 1.0f);
    c.mute = getBool(j, "mute", path, false);
    c.groupId = getString(j, "groupId", path, "");
    c.sends = sendsFromJson(j, "sends", path);
    c.effects = effectsFromJson(j, path);
    c.reconnect = getString(j, "reconnect", path, "auto");
    if (c.reconnect != "auto" && c.reconnect != "manual") c.reconnect = "auto";
    return c;
}

static SceneConfig sceneFromJson(const json& j, const std::string& path) {
    SceneConfig s;
    s.id = getString(j, "id", path, "", true);
    s.name = getString(j, "name", path, "Scene");
    if (auto it = j.find("channels"); it != j.end() && it->is_object()) {
        for (auto& [id, v] : it->items()) {
            const std::string p = path + "/channels/" + id;
            SceneChannelState st;
            st.volume = getFloat(v, "volume", p, 1.0f, 0.0f, kFaderMax);
            st.mute = getBool(v, "mute", p, false);
            if (auto g = v.find("sendGains"); g != v.end() && g->is_object())
                for (auto& [b, gv] : g->items())
                    if (gv.is_number()) st.sendGains[b] = std::clamp(gv.get<float>(), 0.0f, 2.0f);
            if (auto e = v.find("sendEnabled"); e != v.end() && e->is_object())
                for (auto& [b, ev] : e->items())
                    if (ev.is_boolean()) st.sendEnabled[b] = ev.get<bool>();
            s.channels[id] = std::move(st);
        }
    }
    auto readPairs = [&](const char* key, std::map<std::string, std::pair<float, bool>>& out) {
        if (auto it = j.find(key); it != j.end() && it->is_object())
            for (auto& [id, v] : it->items()) {
                const std::string p = path + "/" + key + "/" + id;
                out[id] = {getFloat(v, "volume", p, 1.0f, 0.0f, kFaderMax), getBool(v, "mute", p, false)};
            }
    };
    readPairs("groups", s.groups);
    readPairs("buses", s.buses);
    return s;
}

ProfileConfig profileFromJson(const json& j, const std::string& path) {
    ProfileConfig p;
    p.id = getString(j, "id", path, "", true);
    p.name = getString(j, "name", path, "Profile");
    if (const json* arr = getArray(j, "channels", path))
        for (size_t i = 0; i < arr->size(); ++i)
            p.channels.push_back(channelFromJson((*arr)[i], path + "/channels/" + std::to_string(i)));
    if (const json* arr = getArray(j, "groups", path))
        for (size_t i = 0; i < arr->size(); ++i) {
            const std::string gp = path + "/groups/" + std::to_string(i);
            const json& g = (*arr)[i];
            GroupConfig gc;
            gc.id = getString(g, "id", gp, "", true);
            gc.name = getString(g, "name", gp, "Group");
            gc.volume = getFloat(g, "volume", gp, 1.0f, 0.0f, 1.0f);
            gc.mute = getBool(g, "mute", gp, false);
            gc.collapsed = getBool(g, "collapsed", gp, false);
            p.groups.push_back(std::move(gc));
        }
    if (const json* arr = getArray(j, "buses", path))
        for (size_t i = 0; i < arr->size(); ++i) {
            const std::string bp = path + "/buses/" + std::to_string(i);
            const json& b = (*arr)[i];
            BusConfig bc;
            bc.id = getString(b, "id", bp, "", true);
            bc.name = getString(b, "name", bp, "Bus");
            bc.isMain = getBool(b, "isMain", bp, false);
            bc.volume = getFloat(b, "volume", bp, 1.0f, 0.0f, kFaderMax);
            bc.mute = getBool(b, "mute", bp, false);
            bc.deviceId = getString(b, "deviceId", bp, "");
            bc.deviceName = getString(b, "deviceName", bp, "");
            if (const json* fb = getArray(b, "fallbackDeviceIds", bp))
                for (auto& f : *fb)
                    if (f.is_string()) bc.fallbackDeviceIds.push_back(f.get<std::string>());
            bc.busSends = sendsFromJson(b, "busSends", bp);
            bc.effects = effectsFromJson(b, bp);
            p.buses.push_back(std::move(bc));
        }
    if (const json* arr = getArray(j, "scenes", path))
        for (size_t i = 0; i < arr->size(); ++i)
            p.scenes.push_back(sceneFromJson((*arr)[i], path + "/scenes/" + std::to_string(i)));
    if (auto it = j.find("layout"); it != j.end() && it->is_object()) p.layout = *it;
    return p;
}

AppConfig configFromJson(const json& j) {
    if (!j.is_object()) fail("", "expected object");
    AppConfig c;
    const json& ver = require(j, "schemaVersion", "");
    if (!ver.is_number_integer()) fail("/schemaVersion", "expected integer");
    c.schemaVersion = ver.get<int>();
    if (c.schemaVersion > AppConfig::kSchemaVersion)
        fail("/schemaVersion", "written by a newer dB-Deck (version " + std::to_string(c.schemaVersion) + ")");
    // Migrations from older schema versions would run here. Version 1 is the first.
    c.schemaVersion = AppConfig::kSchemaVersion;
    c.activeProfileId = getString(j, "activeProfileId", "", "");
    if (auto it = j.find("settings"); it != j.end() && it->is_object()) {
        c.settings.bufferMode = getString(*it, "bufferMode", "/settings", "normal");
        if (c.settings.bufferMode != "low" && c.settings.bufferMode != "normal" && c.settings.bufferMode != "safe")
            c.settings.bufferMode = "normal";
        c.settings.meterRateHz = static_cast<int>(getFloat(*it, "meterRateHz", "/settings", 30, 5, 60));
        c.settings.showSystemSounds = getBool(*it, "showSystemSounds", "/settings", false);
    }
    if (const json* arr = getArray(j, "profiles", ""))
        for (size_t i = 0; i < arr->size(); ++i)
            c.profiles.push_back(profileFromJson((*arr)[i], "/profiles/" + std::to_string(i)));
    if (c.profiles.empty()) c.profiles.push_back(makeDefaultProfile("Default"));
    if (!c.findProfile(c.activeProfileId)) c.activeProfileId = c.profiles.front().id;
    for (auto& p : c.profiles) normalizeProfile(p);
    return c;
}

std::vector<std::string> normalizeProfile(ProfileConfig& p) {
    std::vector<std::string> notes;
    // Exactly one Main bus.
    bool seenMain = false;
    for (auto& b : p.buses) {
        if (b.isMain) {
            if (seenMain) {
                b.isMain = false;
                notes.push_back("extra Main bus '" + b.name + "' demoted");
            }
            seenMain = true;
        }
    }
    if (!seenMain) {
        BusConfig m = makeMainBus();
        if (p.findBus(m.id)) m.id = newId();
        p.buses.insert(p.buses.begin(), m);
        notes.push_back("Main bus was missing and has been recreated");
    }
    // Unique IDs.
    std::set<std::string> ids;
    for (auto& c : p.channels)
        if (!ids.insert(c.id).second) {
            c.id = newId();
            ids.insert(c.id);
            notes.push_back("duplicate channel id replaced");
        }
    // References.
    std::set<std::string> busIds, groupIds;
    for (auto& b : p.buses) busIds.insert(b.id);
    for (auto& g : p.groups) groupIds.insert(g.id);
    for (auto& c : p.channels) {
        if (!c.groupId.empty() && !groupIds.count(c.groupId)) {
            c.groupId.clear();
            notes.push_back("channel '" + c.name + "' referenced a missing group");
        }
        auto before = c.sends.size();
        std::set<std::string> seen;
        std::erase_if(c.sends, [&](const SendConfig& s) { return !busIds.count(s.busId) || !seen.insert(s.busId).second; });
        if (c.sends.size() != before) notes.push_back("channel '" + c.name + "' had invalid sends");
    }
    for (auto& b : p.buses) {
        std::set<std::string> seen;
        std::erase_if(b.busSends, [&](const SendConfig& s) {
            return !busIds.count(s.busId) || s.busId == b.id || !seen.insert(s.busId).second;
        });
    }
    return notes;
}

}  // namespace dbdeck
