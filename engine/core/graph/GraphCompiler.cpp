#include "graph/GraphCompiler.h"

#include <set>

#include "graph/Validation.h"

namespace dbdeck {

std::shared_ptr<ChannelRuntime> RuntimeRegistry::channel(const std::string& id) {
    auto& p = channels_[id];
    if (!p) p = std::make_shared<ChannelRuntime>();
    return p;
}

std::shared_ptr<BusRuntime> RuntimeRegistry::bus(const std::string& id) {
    auto& p = buses_[id];
    if (!p) p = std::make_shared<BusRuntime>();
    return p;
}

std::shared_ptr<SendRuntime> RuntimeRegistry::send(const std::string& fromId, const std::string& toBusId) {
    auto& p = sends_[fromId + "->" + toBusId];
    if (!p) p = std::make_shared<SendRuntime>();
    return p;
}

std::shared_ptr<Effect> RuntimeRegistry::effect(const EffectConfig& cfg) {
    auto type = effectTypeFromString(cfg.type);
    if (!type) return nullptr;
    auto& p = effects_[cfg.id];
    if (!p || p->type() != *type) p = std::shared_ptr<Effect>(createEffect(*type).release());
    return p;
}

ChannelRuntime* RuntimeRegistry::findChannel(const std::string& id) const {
    auto it = channels_.find(id);
    return it == channels_.end() ? nullptr : it->second.get();
}
BusRuntime* RuntimeRegistry::findBus(const std::string& id) const {
    auto it = buses_.find(id);
    return it == buses_.end() ? nullptr : it->second.get();
}
Effect* RuntimeRegistry::findEffect(const std::string& id) const {
    auto it = effects_.find(id);
    return it == effects_.end() ? nullptr : it->second.get();
}

float channelGain(const ProfileConfig& profile, const ChannelConfig& c) {
    if (c.mute) return 0.0f;
    float g = faderToGain(c.volume) * dbToLinear(c.gainDb);
    if (!c.groupId.empty())
        if (const GroupConfig* grp = profile.findGroup(c.groupId)) {
            if (grp->mute) return 0.0f;
            g *= faderToGain(grp->volume);
        }
    return g;
}

static void applyEffects(RuntimeRegistry& reg, const std::vector<EffectConfig>& effects) {
    for (auto& e : effects) {
        auto fx = reg.effect(e);
        if (!fx) continue;
        for (auto& [k, v] : e.params) fx->setParam(k, v);
        fx->setEnabled(e.enabled);
    }
}

void RuntimeRegistry::applyParams(const ProfileConfig& profile) {
    const BusConfig* main = profile.mainBus();
    for (auto& c : profile.channels) {
        auto rt = channel(c.id);
        rt->gain.setTarget(channelGain(profile, c));
        rt->balance.setTarget(c.balance);
        const bool soloedOut = !solo.empty() && !solo.count(c.id);
        for (auto& s : c.sends) {
            const bool cut = soloedOut && main && s.busId == main->id;
            send(c.id, s.busId)->gain.setTarget(s.enabled && !cut ? s.gain : 0.0f);
        }
        applyEffects(*this, c.effects);
    }
    for (auto& b : profile.buses) {
        bus(b.id)->gain.setTarget(b.mute ? 0.0f : faderToGain(b.volume));
        for (auto& s : b.busSends) send(b.id, s.busId)->gain.setTarget(s.enabled ? s.gain : 0.0f);
        applyEffects(*this, b.effects);
    }
}

void RuntimeRegistry::prune(const ProfileConfig& profile) {
    std::set<std::string> ch, bu, se, fx;
    for (auto& c : profile.channels) {
        ch.insert(c.id);
        for (auto& s : c.sends) se.insert(c.id + "->" + s.busId);
        for (auto& e : c.effects) fx.insert(e.id);
    }
    for (auto& b : profile.buses) {
        bu.insert(b.id);
        for (auto& s : b.busSends) se.insert(b.id + "->" + s.busId);
        for (auto& e : b.effects) fx.insert(e.id);
    }
    // Graphs still in flight hold their own shared_ptr copies, so erasing
    // here never frees memory the audio thread is using.
    std::erase_if(channels_, [&](auto& kv) { return !ch.count(kv.first); });
    std::erase_if(buses_, [&](auto& kv) { return !bu.count(kv.first); });
    std::erase_if(sends_, [&](auto& kv) { return !se.count(kv.first); });
    std::erase_if(effects_, [&](auto& kv) { return !fx.count(kv.first); });
    std::erase_if(solo, [&](const std::string& id) { return !ch.count(id); });
}

std::unique_ptr<CompiledGraph> compileGraph(const ProfileConfig& profile, RuntimeRegistry& reg,
                                            const CompileContext& ctx, uint64_t epoch) {
    auto g = std::make_unique<CompiledGraph>();
    g->epoch = epoch;

    // Bus order (validation guarantees acyclic; fall back to declaration order).
    BusOrder order = orderBuses(profile);
    std::vector<std::string> busIds = order.order;
    if (!order.cycle.empty()) {
        busIds.clear();
        for (auto& b : profile.buses) busIds.push_back(b.id);
    }
    std::map<std::string, uint32_t> busIndex;
    for (auto& id : busIds) {
        busIndex[id] = static_cast<uint32_t>(g->buses.size());
        CompiledBus cb;
        auto rt = reg.bus(id);
        cb.rt = rt.get();
        g->keepAlive.push_back(rt);
        g->buses.push_back(std::move(cb));
    }

    // Bus inputs from other buses, and bus effects.
    for (auto& b : profile.buses) {
        for (auto& s : b.busSends) {
            if (!s.enabled || !order.cycle.empty()) continue;
            auto to = busIndex.find(s.busId);
            if (to == busIndex.end()) continue;
            auto rt = reg.send(b.id, s.busId);
            g->keepAlive.push_back(rt);
            g->buses[to->second].inputs.push_back({busIndex[b.id], rt.get(), s.preFader});
        }
        auto& cb = g->buses[busIndex[b.id]];
        for (auto& e : b.effects) {
            auto fx = reg.effect(e);
            if (!fx) continue;
            g->keepAlive.push_back(fx);
            cb.effects.push_back(fx.get());
        }
    }

    // Channels carrying managed PCM.
    for (auto& c : profile.channels) {
        auto rt = reg.channel(c.id);
        g->keepAlive.push_back(rt);
        std::vector<std::shared_ptr<PullSource>> sources = ctx.sourcesFor ? ctx.sourcesFor(c) : decltype(sources){};
        if (sources.empty()) continue;
        CompiledChannel cc;
        cc.rt = rt.get();
        for (auto& s : sources) {
            g->keepAlive.push_back(s);
            cc.inputs.push_back(s.get());
        }
        for (auto& e : c.effects) {
            auto fx = reg.effect(e);
            if (!fx) continue;
            g->keepAlive.push_back(fx);
            cc.effects.push_back(fx.get());
        }
        for (auto& s : c.sends) {
            if (!s.enabled) continue;
            auto bi = busIndex.find(s.busId);
            if (bi == busIndex.end()) continue;
            const BusConfig* bus = profile.findBus(s.busId);
            if (ctx.sendBlockedReason && !ctx.sendBlockedReason(c, s, *bus).empty()) continue;
            auto srt = reg.send(c.id, s.busId);
            g->keepAlive.push_back(srt);
            cc.sends.push_back({bi->second, srt.get(), s.preFader});
        }
        g->channels.push_back(std::move(cc));
    }

    // Outputs: group buses by destination device.
    std::map<std::string, size_t> outIndex;
    for (auto& id : busIds) {
        const BusConfig* b = profile.findBus(id);
        if (!b || !ctx.outputFor) continue;
        auto ob = ctx.outputFor(*b);
        if (!ob) continue;
        auto it = outIndex.find(ob->deviceId);
        if (it == outIndex.end()) {
            OutputTarget t;
            t.deviceId = ob->deviceId;
            t.ring = ob->ring.get();
            if (ob->ring) g->keepAlive.push_back(ob->ring);
            outIndex[ob->deviceId] = g->outputs.size();
            g->outputs.push_back(std::move(t));
            it = outIndex.find(ob->deviceId);
        }
        g->outputs[it->second].buses.push_back(busIndex[id]);
    }

    g->allocate();
    return g;
}

}  // namespace dbdeck
