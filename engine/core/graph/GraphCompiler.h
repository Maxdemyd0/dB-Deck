#pragma once
// Compiles the declarative ProfileConfig into a CompiledGraph for the Mixer.
//
// RuntimeRegistry owns the long-lived per-entity runtime objects (gain
// smoothers, meters, effect processors) keyed by stable IDs, so successive
// graphs share them. applyParams() pushes continuous parameters from the
// model into those atomics without rebuilding anything.

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "graph/Mixer.h"
#include "graph/Model.h"

namespace dbdeck {

class RuntimeRegistry {
public:
    std::shared_ptr<ChannelRuntime> channel(const std::string& id);
    std::shared_ptr<BusRuntime> bus(const std::string& id);
    std::shared_ptr<SendRuntime> send(const std::string& fromId, const std::string& toBusId);
    std::shared_ptr<Effect> effect(const EffectConfig& cfg);

    // Sets every continuous parameter target from the model.
    void applyParams(const ProfileConfig& profile);
    // Drops runtimes for entities no longer in the profile.
    void prune(const ProfileConfig& profile);

    ChannelRuntime* findChannel(const std::string& id) const;
    BusRuntime* findBus(const std::string& id) const;
    Effect* findEffect(const std::string& id) const;

    // Soloed channel IDs (runtime only, never saved). While any channel is
    // soloed, other channels' sends to the Main bus are silenced; sends to
    // every other bus (virtual mics, recordings) are left alone.
    std::set<std::string> solo;

private:
    std::map<std::string, std::shared_ptr<ChannelRuntime>> channels_;
    std::map<std::string, std::shared_ptr<BusRuntime>> buses_;
    std::map<std::string, std::shared_ptr<SendRuntime>> sends_;  // key: from + "->" + to
    std::map<std::string, std::shared_ptr<Effect>> effects_;
};

// Effective linear gain of a channel's fader stage (fader, trim, group, mutes).
float channelGain(const ProfileConfig& profile, const ChannelConfig& c);

struct OutputBinding {
    std::string deviceId;
    std::shared_ptr<AdaptiveStream> ring;  // nullptr: rendered directly by the clock device
};

struct CompileContext {
    // PCM sources feeding a channel. Empty: the channel carries no managed
    // audio (e.g. a session-control-only App channel, or an offline device).
    std::function<std::vector<std::shared_ptr<PullSource>>(const ChannelConfig&)> sourcesFor;
    // Non-empty reason: this send must not carry audio (e.g. double-audio guard).
    std::function<std::string(const ChannelConfig&, const SendConfig&, const BusConfig&)> sendBlockedReason;
    // Where a bus's audio goes. nullopt: bus has no connected destination.
    std::function<std::optional<OutputBinding>(const BusConfig&)> outputFor;
};

std::unique_ptr<CompiledGraph> compileGraph(const ProfileConfig& profile, RuntimeRegistry& registry,
                                            const CompileContext& ctx, uint64_t epoch);

}  // namespace dbdeck
