#pragma once
#include <optional>
#include <string>
#include <vector>

#include "graph/Model.h"

namespace dbdeck {

struct RoutingIssue {
    std::string code;     // "cycle" | "feedback"
    std::string message;  // human-readable explanation
};

struct BusOrder {
    std::vector<std::string> order;  // topological processing order (sources first)
    std::vector<std::string> cycle;  // non-empty if bus sends form a cycle
};

// Orders buses so every bus is processed after all buses that send into it.
BusOrder orderBuses(const ProfileConfig& profile);

// Buses reachable from `busIds` through enabled bus-to-bus sends (inclusive).
std::vector<std::string> reachableBuses(const ProfileConfig& profile, const std::vector<std::string>& busIds);

// Rejects configurations that would create a routing cycle, or a feedback
// loop where a loopback capture of a playback endpoint is routed back into a
// bus that plays on that same endpoint.
std::optional<RoutingIssue> validateRouting(const ProfileConfig& profile);

}  // namespace dbdeck
