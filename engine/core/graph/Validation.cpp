#include "graph/Validation.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace dbdeck {

BusOrder orderBuses(const ProfileConfig& profile) {
    BusOrder result;
    // Edges: from -> to for every enabled bus send.
    std::map<std::string, std::vector<std::string>> edges;
    for (auto& b : profile.buses)
        for (auto& s : b.busSends)
            if (s.enabled && profile.findBus(s.busId)) edges[b.id].push_back(s.busId);

    enum Mark { None, Visiting, Done };
    std::map<std::string, Mark> mark;
    std::vector<std::string> stack;
    std::vector<std::string> post;

    std::function<bool(const std::string&)> visit = [&](const std::string& id) -> bool {
        if (mark[id] == Done) return true;
        if (mark[id] == Visiting) {
            auto it = std::find(stack.begin(), stack.end(), id);
            result.cycle.assign(it, stack.end());
            result.cycle.push_back(id);
            return false;
        }
        mark[id] = Visiting;
        stack.push_back(id);
        for (auto& to : edges[id])
            if (!visit(to)) return false;
        stack.pop_back();
        mark[id] = Done;
        post.push_back(id);
        return true;
    };

    for (auto& b : profile.buses)
        if (!visit(b.id)) return result;
    // Reverse post-order puts senders before receivers.
    result.order.assign(post.rbegin(), post.rend());
    return result;
}

std::vector<std::string> reachableBuses(const ProfileConfig& profile, const std::vector<std::string>& busIds) {
    std::set<std::string> seen;
    std::vector<std::string> queue(busIds.begin(), busIds.end());
    std::vector<std::string> out;
    while (!queue.empty()) {
        std::string id = queue.back();
        queue.pop_back();
        if (!seen.insert(id).second) continue;
        const BusConfig* b = profile.findBus(id);
        if (!b) continue;
        out.push_back(id);
        for (auto& s : b->busSends)
            if (s.enabled) queue.push_back(s.busId);
    }
    return out;
}

static std::string busName(const ProfileConfig& p, const std::string& id) {
    const BusConfig* b = p.findBus(id);
    return b ? b->name : id;
}

std::optional<RoutingIssue> validateRouting(const ProfileConfig& profile, const CablePairs& pairs) {
    BusOrder order = orderBuses(profile);
    if (!order.cycle.empty()) {
        std::string path;
        for (size_t i = 0; i < order.cycle.size(); ++i) {
            if (i) path += " -> ";
            path += busName(profile, order.cycle[i]);
        }
        return RoutingIssue{"cycle", "Routing cycle: " + path};
    }

    for (auto& c : profile.channels) {
        if (c.deviceId.empty()) continue;
        // The playback endpoints this input hears, if any.
        std::vector<std::string> heard;
        if (c.type == ChannelType::EndpointLoopback) heard.push_back(c.deviceId);
        else if (c.type == ChannelType::HardwareInput)
            if (auto it = pairs.find(c.deviceId); it != pairs.end()) heard = it->second;
        if (heard.empty()) continue;
        std::vector<std::string> direct;
        for (auto& s : c.sends)
            if (s.enabled) direct.push_back(s.busId);
        for (auto& id : reachableBuses(profile, direct)) {
            const BusConfig* b = profile.findBus(id);
            if (!b) continue;
            const bool hits = std::any_of(heard.begin(), heard.end(), [&](const std::string& h) {
                return b->deviceId == h ||
                       std::find(b->fallbackDeviceIds.begin(), b->fallbackDeviceIds.end(), h) != b->fallbackDeviceIds.end();
            });
            if (hits)
                return RoutingIssue{"feedback", "Feedback loop: '" + c.name + "' hears the device that bus '" +
                                                    b->name + "' plays to"};
        }
    }
    return std::nullopt;
}

}  // namespace dbdeck
