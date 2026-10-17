#pragma once
// Scenes: quick level/mute/send snapshots inside a profile. Applying a scene
// only changes continuous parameters, never sources, buses or devices, so it
// switches smoothly without touching hardware streams.

#include "graph/Model.h"

namespace dbdeck {

inline SceneConfig captureScene(const ProfileConfig& p, const std::string& id, const std::string& name) {
    SceneConfig s;
    s.id = id;
    s.name = name;
    for (auto& c : p.channels) {
        SceneChannelState st;
        st.volume = c.volume;
        st.mute = c.mute;
        for (auto& send : c.sends) {
            st.sendGains[send.busId] = send.gain;
            st.sendEnabled[send.busId] = send.enabled;
        }
        s.channels[c.id] = std::move(st);
    }
    for (auto& g : p.groups) s.groups[g.id] = {g.volume, g.mute};
    for (auto& b : p.buses) s.buses[b.id] = {b.volume, b.mute};
    return s;
}

// Applies a scene. Entities that no longer exist are ignored; entities added
// after the scene was captured keep their current values. Send enable flags
// only toggle sends that already exist (routing structure is not changed).
inline void applyScene(ProfileConfig& p, const SceneConfig& s) {
    for (auto& c : p.channels) {
        auto it = s.channels.find(c.id);
        if (it == s.channels.end()) continue;
        c.volume = it->second.volume;
        c.mute = it->second.mute;
        for (auto& send : c.sends) {
            if (auto g = it->second.sendGains.find(send.busId); g != it->second.sendGains.end()) send.gain = g->second;
            if (auto e = it->second.sendEnabled.find(send.busId); e != it->second.sendEnabled.end())
                send.enabled = e->second;
        }
    }
    for (auto& g : p.groups)
        if (auto it = s.groups.find(g.id); it != s.groups.end()) {
            g.volume = it->second.first;
            g.mute = it->second.second;
        }
    for (auto& b : p.buses)
        if (auto it = s.buses.find(b.id); it != s.buses.end()) {
            b.volume = it->second.first;
            b.mute = it->second.second;
        }
}

}  // namespace dbdeck
