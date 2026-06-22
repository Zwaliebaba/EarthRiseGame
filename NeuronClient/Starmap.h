#pragma once
// Starmap — client-side navigation graph helper (§13.12, §23.1): solve a route
// across the jump-beacon network so the starmap UI can "set destination" and the
// autopilot can hop beacon-to-beacon. Pure graph logic over the cooked
// UniverseDataset (same data the server loads), platform-independent so it builds
// and tests on Linux. The EarthRise app renders the graph + issues the per-hop
// Jump intents (area B); this just computes the path / reachable set.

#include "UniverseData.h" // UniverseDataset, BeaconDef (NeuronCore)

#include <algorithm>
#include <cstddef>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Neuron::Client
{

// Shortest beacon route (fewest hops) from 'src' to 'dst' over the public link
// graph, inclusive of both endpoints. Empty if either name is unknown or no path
// exists. Breadth-first → minimum-hop path; deterministic tie-break by link order.
[[nodiscard]] inline std::vector<std::string>
SolveBeaconRoute(const Neuron::Sim::UniverseDataset& uni,
                 const std::string& src, const std::string& dst)
{
    if (!uni.FindBeacon(src) || !uni.FindBeacon(dst)) return {};
    if (src == dst) return { src };

    std::unordered_map<std::string, std::string> cameFrom; // node → predecessor
    std::unordered_set<std::string> visited{ src };
    std::deque<std::string> frontier{ src };

    while (!frontier.empty()) {
        const std::string cur = frontier.front();
        frontier.pop_front();
        const Neuron::Sim::BeaconDef* b = uni.FindBeacon(cur);
        if (!b) continue;
        for (const std::string& nbr : b->links) {
            if (visited.count(nbr)) continue;
            visited.insert(nbr);
            cameFrom[nbr] = cur;
            if (nbr == dst) {
                // Reconstruct.
                std::vector<std::string> path{ dst };
                for (std::string n = dst; n != src; ) { n = cameFrom[n]; path.push_back(n); }
                std::reverse(path.begin(), path.end());
                return path;
            }
            frontier.push_back(nbr);
        }
    }
    return {}; // unreachable
}

// Every beacon reachable from 'src' (inclusive). Useful for greying out
// unreachable destinations on the starmap.
[[nodiscard]] inline std::unordered_set<std::string>
ReachableBeacons(const Neuron::Sim::UniverseDataset& uni, const std::string& src)
{
    std::unordered_set<std::string> reached;
    if (!uni.FindBeacon(src)) return reached;
    std::deque<std::string> frontier{ src };
    reached.insert(src);
    while (!frontier.empty()) {
        const std::string cur = frontier.front();
        frontier.pop_front();
        const Neuron::Sim::BeaconDef* b = uni.FindBeacon(cur);
        if (!b) continue;
        for (const std::string& nbr : b->links)
            if (uni.FindBeacon(nbr) && reached.insert(nbr).second) frontier.push_back(nbr);
    }
    return reached;
}

} // namespace Neuron::Client
