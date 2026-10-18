#include "config/VoicemeeterImport.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace dbdeck {

namespace {

using Attrs = std::map<std::string, std::string>;

// Minimal scanner for the flat, attribute-only elements Voicemeeter writes.
// Not a general XML parser: no entities beyond &amp; &quot; &apos; &lt; &gt;.
std::string unescape(std::string s) {
    static const std::pair<const char*, const char*> kEnt[] = {
        {"&quot;", "\""}, {"&apos;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}};
    for (auto& [from, to] : kEnt) {
        size_t p = 0;
        while ((p = s.find(from, p)) != std::string::npos) {
            s.replace(p, std::strlen(from), to);
            p += std::strlen(to);
        }
    }
    return s;
}

std::vector<Attrs> elements(const std::string& xml, const std::string& tag) {
    std::vector<Attrs> out;
    const std::string open = "<" + tag;
    size_t pos = 0;
    while ((pos = xml.find(open, pos)) != std::string::npos) {
        size_t i = pos + open.size();
        pos = i;
        if (i >= xml.size() || !(xml[i] == ' ' || xml[i] == '\t' || xml[i] == '/' || xml[i] == '>')) continue;
        const size_t end = xml.find('>', i);
        if (end == std::string::npos) break;
        Attrs a;
        while (i < end) {
            while (i < end && (xml[i] == ' ' || xml[i] == '\t' || xml[i] == '\r' || xml[i] == '\n')) ++i;
            const size_t eq = xml.find('=', i);
            if (eq == std::string::npos || eq >= end) break;
            std::string key = xml.substr(i, eq - i);
            while (!key.empty() && key.back() == ' ') key.pop_back();
            size_t q = eq + 1;
            while (q < end && xml[q] == ' ') ++q;
            if (q >= end || (xml[q] != '\'' && xml[q] != '"')) break;
            const char quote = xml[q];
            const size_t close = xml.find(quote, q + 1);
            if (close == std::string::npos || close > end) break;
            a[key] = unescape(xml.substr(q + 1, close - q - 1));
            i = close + 1;
        }
        out.push_back(std::move(a));
        pos = end;
    }
    return out;
}

std::string elementText(const std::string& xml, const std::string& tag) {
    const std::string open = "<" + tag + ">", close = "</" + tag + ">";
    const size_t a = xml.find(open);
    if (a == std::string::npos) return {};
    const size_t b = xml.find(close, a);
    if (b == std::string::npos) return {};
    return unescape(xml.substr(a + open.size(), b - a - open.size()));
}

int toInt(const Attrs& a, const char* key, int def = 0) {
    auto it = a.find(key);
    return it == a.end() ? def : std::atoi(it->second.c_str());
}

double toDouble(const Attrs& a, const char* key, double def = 0.0) {
    auto it = a.find(key);
    return it == a.end() ? def : std::atof(it->second.c_str());
}

std::string get(const Attrs& a, const char* key) {
    auto it = a.find(key);
    return it == a.end() ? std::string() : it->second;
}

// First element per index that carries `marker` (Voicemeeter repeats Strip /
// Bus elements with different attribute sets and in preset memories).
std::map<int, Attrs> firstByIndex(const std::vector<Attrs>& els, const char* marker) {
    std::map<int, Attrs> out;
    for (auto& e : els) {
        if (!e.count(marker) || !e.count("index")) continue;
        const int idx = toInt(e, "index");
        if (idx > 0 && !out.count(idx)) out[idx] = e;
    }
    return out;
}

const ImportDevice* findDevice(const std::vector<ImportDevice>& devices, const std::string& name, bool render) {
    if (name.empty() || name == "-") return nullptr;
    for (auto& d : devices)
        if (d.render == render && d.name == name) return &d;
    // MME names are truncated to 31 characters.
    for (auto& d : devices)
        if (d.render == render && name.size() >= 20 && d.name.rfind(name, 0) == 0) return &d;
    return nullptr;
}

const ImportDevice* findVirtualInput(const std::vector<ImportDevice>& devices, const std::string& base) {
    for (auto& d : devices)
        if (d.render && d.name.rfind(base + " (", 0) == 0) return &d;
    return nullptr;
}

std::string busKey(bool aBus, int k) {
    // busa, busa2..busa5 / busb, busb2..busb3
    std::string s = aBus ? "busa" : "busb";
    if (k > 1) s += std::to_string(k);
    return s;
}

std::string fmtDb(double db) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%+.1f dB", db);
    return buf;
}

}  // namespace

VoicemeeterImportResult importVoicemeeter(const std::string& xml, const std::vector<ImportDevice>& devices,
                                          const std::string& profileName) {
    if (xml.find("VBAudioVoicemeeterSettings") == std::string::npos)
        throw ConfigError("not a Voicemeeter settings file");

    VoicemeeterImportResult r;
    auto inputs = firstByIndex(elements(xml, "InputDev"), "name");
    auto outputs = firstByIndex(elements(xml, "OutputDev"), "name");
    auto strips = firstByIndex(elements(xml, "Strip"), "busa");
    auto buses = firstByIndex(elements(xml, "Bus"), "mute");
    if (strips.empty() || buses.empty()) throw ConfigError("no mixer strips or buses found in the file");

    const int hwStrips = static_cast<int>(inputs.size());
    const int aBuses = static_cast<int>(outputs.size());
    const int virtStrips = std::max(0, strips.rbegin()->first - hwStrips);
    const int bBuses = std::max(0, buses.rbegin()->first - aBuses);

    std::vector<std::string> virtualNames;
    if (hwStrips >= 5) {
        r.edition = "Potato";
        virtualNames = {"Voicemeeter Input", "Voicemeeter AUX Input", "Voicemeeter VAIO3 Input"};
    } else if (hwStrips >= 3) {
        r.edition = "Banana";
        virtualNames = {"Voicemeeter Input", "Voicemeeter AUX Input"};
    } else {
        r.edition = "Standard";
        virtualNames = {"Voicemeeter Input"};
    }

    ProfileConfig p = makeDefaultProfile(profileName);

    // --- buses ---------------------------------------------------------------
    std::map<std::string, std::string> busIdFor;  // "A1" / "B2" -> dB-Deck bus id
    auto busLevel = [&](BusConfig& b, const Attrs& a, const std::string& vmName) {
        const double db = toDouble(a, "dblevel");
        const double lin = std::pow(10.0, std::min(db, 0.0) / 20.0);
        b.volume = static_cast<float>(std::clamp(std::sqrt(lin), 0.0, 1.0));
        b.mute = toInt(a, "mute") != 0;
        if (db > 0.05)
            r.notes.push_back(vmName + " was boosted " + fmtDb(db) + "; dB-Deck bus faders stop at 0 dB, so it is set to 0 dB.");
    };
    bool mainAssigned = false;
    for (int k = 1; k <= aBuses; ++k) {
        const std::string vm = "A" + std::to_string(k);
        const std::string devName = get(outputs[k], "name");
        if (devName.empty() || devName == "-") continue;
        const std::string label = elementText(xml, "LabelBus" + std::to_string(k));
        const ImportDevice* dev = findDevice(devices, devName, true);
        BusConfig* b;
        if (!mainAssigned) {
            b = p.mainBus();
            mainAssigned = true;
            r.notes.push_back(vm + " (" + devName + ") became the Main bus.");
        } else {
            BusConfig nb;
            nb.id = newId();
            nb.name = !label.empty() ? label : vm;
            p.buses.push_back(nb);
            b = &p.buses.back();
        }
        b->deviceId = dev ? dev->id : "";
        b->deviceName = devName;
        if (!dev) r.notes.push_back(vm + " output \"" + devName + "\" is not connected right now; it will be used when it appears.");
        if (buses.count(k)) busLevel(*b, buses[k], vm);
        busIdFor[vm] = b->id;
    }
    if (!mainAssigned) r.notes.push_back("No A output had a device, so Main has no output yet. Choose one in Outputs.");

    // B buses: only those something is routed to.
    for (int k = 1; k <= bBuses; ++k) {
        const std::string vm = "B" + std::to_string(k);
        const std::string key = busKey(false, k);
        const bool used = std::any_of(strips.begin(), strips.end(), [&](auto& s) { return toInt(s.second, key.c_str()) != 0; });
        if (!used) continue;
        const std::string label = elementText(xml, "LabelVirtualBus" + std::to_string(k));
        BusConfig nb;
        nb.id = newId();
        nb.name = !label.empty() ? label : vm + " (virtual mic)";
        const int idx = aBuses + k;
        if (buses.count(idx)) busLevel(nb, buses[idx], vm);
        p.buses.push_back(nb);
        busIdFor[vm] = nb.id;
        r.notes.push_back(vm + " needs a virtual cable: set \"" + nb.name +
                          "\"'s output to a cable's playback side (e.g. CABLE Input) and select the cable's recording side "
                          "(e.g. CABLE Output) where you used \"Voicemeeter Out " + vm + "\".");
    }

    // --- strips ------------------------------------------------------------------
    std::vector<std::string> ignoredFeatures;
    for (int s = 1; s <= hwStrips + virtStrips; ++s) {
        if (!strips.count(s)) continue;
        const Attrs& st = strips[s];
        const bool isVirtual = s > hwStrips;
        const std::string vm = isVirtual ? "virtual input " + std::to_string(s - hwStrips) : "hardware input " + std::to_string(s);

        std::vector<std::string> targets;
        for (int k = 1; k <= 5; ++k)
            if (toInt(st, busKey(true, k).c_str())) targets.push_back("A" + std::to_string(k));
        for (int k = 1; k <= 3; ++k)
            if (toInt(st, busKey(false, k).c_str())) targets.push_back("B" + std::to_string(k));

        ChannelConfig c;
        if (isVirtual) {
            const int v = s - hwStrips;
            if (v > static_cast<int>(virtualNames.size())) continue;
            if (targets.empty()) continue;  // unused app input
            const std::string base = virtualNames[v - 1];
            const std::string label = elementText(xml, "LabelVirtualStrip" + std::to_string(v));
            c = makeChannel(ChannelType::EndpointLoopback, !label.empty() ? label : base, p);
            const ImportDevice* dev = findVirtualInput(devices, base);
            c.deviceId = dev ? dev->id : "";
            c.deviceName = dev ? dev->name : base;
            if (!dev) r.notes.push_back("\"" + base + "\" was not found; apps playing there cannot be captured.");
        } else {
            const std::string devName = get(inputs[s], "name");
            if (devName.empty() || devName == "-") continue;
            if (targets.empty()) {
                r.notes.push_back("\"" + devName + "\" (" + vm + ") was not routed anywhere and was skipped.");
                continue;
            }
            const std::string label = elementText(xml, "LabelStrip" + std::to_string(s));
            c = makeChannel(ChannelType::HardwareInput, !label.empty() ? label : devName, p);
            const ImportDevice* dev = findDevice(devices, devName, false);
            c.deviceId = dev ? dev->id : "";
            c.deviceName = devName;
            if (!dev) r.notes.push_back("\"" + devName + "\" is not connected right now; it will be used when it appears.");
        }
        c.volume = 1.0f;
        c.gainDb = static_cast<float>(std::clamp(toDouble(st, "dblevel"), -60.0, 24.0));
        c.mute = toInt(st, "mute") != 0;
        c.sends.clear();
        for (auto& t : targets) {
            auto it = busIdFor.find(t);
            if (it == busIdFor.end()) {
                r.notes.push_back("\"" + c.name + "\" was routed to " + t + ", which has no device; that route was skipped.");
                continue;
            }
            c.sends.push_back(SendConfig{it->second, true, 1.0f, false});
        }
        if (toInt(st, "mono")) ignoredFeatures.push_back(c.name + ": mono");
        if (toInt(st, "EQon")) ignoredFeatures.push_back(c.name + ": EQ");
        p.channels.push_back(std::move(c));
    }

    // Strip processing that is on in Voicemeeter but not imported.
    for (auto& comp : firstByIndex(elements(xml, "StripComp"), "comprate"))
        if (toDouble(comp.second, "comprate", 1.0) > 1.01 && comp.first <= hwStrips + virtStrips)
            ignoredFeatures.push_back("strip " + std::to_string(comp.first) + ": compressor");
    for (auto& b : buses)
        if (toInt(b.second, "EQon")) ignoredFeatures.push_back("bus " + std::to_string(b.first) + ": EQ");
    if (!ignoredFeatures.empty()) {
        std::string list;
        for (auto& f : ignoredFeatures) list += (list.empty() ? "" : ", ") + f;
        r.notes.push_back("Not imported (add dB-Deck effects instead if needed): " + list + ".");
    }
    r.notes.push_back("Close Voicemeeter (and turn off its \"run on startup\") before loading this profile, or you will hear audio twice.");

    normalizeProfile(p);
    r.profile = std::move(p);
    return r;
}

}  // namespace dbdeck
