#pragma once
// UniverseData.h — the cooked universe layout: regions + security tiers, the
// jump-beacon graph, and resource fields (masterplan §12.6, §13.5–13.12;
// schema in docs/design/universe-worldgen.md §4.1–4.3).
//
// Authored as text → cooked to versioned binary by NeuronTools/datacook →
// validated by datacheck → loaded here at runtime. Pure data + Serde, no
// platform deps (like ShapeCatalog), so the cook tools, the server sim, the
// client, and bots all share ONE format and ONE set of rules (no drift).
//
// This header owns three things:
//   1. the runtime model (RegionDef / BeaconDef / ResourceFieldDef / UniverseDataset),
//   2. the binary codec (EncodeUniverseDataset / DecodeUniverseDataset),
//   3. the referential-integrity rules (ValidateUniverseDataset) — i.e. what
//      `datacheck` enforces. The CLI tools are thin wrappers over these.

#include "Serde.h"
#include "UniversePos.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Neuron::Sim
{

// --- enumerations (wire values are stable; cooked into the binary) ----------

enum class SecurityTier : uint8_t { High = 0, Low = 1, Null = 2 };
enum class BeaconKind   : uint8_t { Public = 0, Claimable = 1 };
enum class ResourceType : uint8_t { Ore = 0, Ice = 1, Gas = 2 };
inline constexpr uint8_t kResourceTypeCount = 3;

[[nodiscard]] inline std::string_view ToString(SecurityTier t) noexcept
{
    switch (t) { case SecurityTier::High: return "high";
                 case SecurityTier::Low:  return "low";
                 case SecurityTier::Null: return "null"; }
    return "?";
}
[[nodiscard]] inline std::string_view ToString(BeaconKind k) noexcept
{
    return k == BeaconKind::Public ? "public" : "claimable";
}
[[nodiscard]] inline std::string_view ToString(ResourceType r) noexcept
{
    switch (r) { case ResourceType::Ore: return "Ore";
                 case ResourceType::Ice: return "Ice";
                 case ResourceType::Gas: return "Gas"; }
    return "?";
}

// --- runtime model ----------------------------------------------------------

// Inclusive SectorId range box (§4.1 `bounds`). Used at runtime to map a sector
// to its region/security tier.
struct SectorBounds
{
    int64_t x0{ 0 }, x1{ 0 }, y0{ 0 }, y1{ 0 }, z0{ 0 }, z1{ 0 };

    [[nodiscard]] bool WellFormed() const noexcept { return x0 <= x1 && y0 <= y1 && z0 <= z1; }

    [[nodiscard]] bool Contains(const Neuron::Universe::SectorId& s) const noexcept
    {
        return s.x >= x0 && s.x <= x1 && s.y >= y0 && s.y <= y1 && s.z >= z0 && s.z <= z1;
    }
};

struct RegionDef
{
    std::string  name;
    SecurityTier security{ SecurityTier::High };
    SectorBounds bounds{};
    float        yieldMult{ 1.0f };
};

struct BeaconDef
{
    std::string                   name;
    std::string                   region; // ref → RegionDef.name
    Neuron::Universe::UniversePos pos{};
    std::vector<std::string>      links;  // refs → BeaconDef.name (must be reciprocal)
    BeaconKind                    kind{ BeaconKind::Public };
};

struct ResourceWeight { ResourceType type{ ResourceType::Ore }; float weight{ 0.0f }; };

struct ResourceFieldDef
{
    std::string                   name;
    std::string                   region; // ref → RegionDef.name
    Neuron::Universe::UniversePos center{};
    float                         radius{ 0.0f };
    std::vector<ResourceWeight>   nodes;  // composition weights (sum ≈ 1.0)
    uint16_t                      countMin{ 0 }, countMax{ 0 };
    int32_t                       yieldMin{ 0 }, yieldMax{ 0 };
    uint32_t                      respawnSeconds{ 0 };
};

// Navigation balance (§13.12). First-pass numbers, tunable as game data and
// validated with bots (§12.6/§19) — kept here, NOT hard-coded in the nav rules.
// Cooked from an optional `tuning { ... }` block; these defaults apply if absent.
struct NavTuning
{
    float warpSpeedShip{ 6000.0f };     // m/s, high sublight (ship)
    float warpSpeedBase{ 2000.0f };     // the mobile base warps slower
    float warpAlignSeconds{ 3.0f };     // align time before warp engages
    float jumpFuelShip{ 20.0f };        // fuel per jump (ship)
    float jumpFuelBase{ 60.0f };        // the base costs more
    float jumpSpoolShip{ 5.0f };        // vulnerability window before a ship jump fires
    float jumpSpoolBase{ 12.0f };       // longer for the base
    float jumpCooldownSeconds{ 10.0f }; // post-jump cooldown
    float beaconUseRange{ 2000.0f };    // must be within this of a beacon to jump from it
    float shipFuelMax{ 100.0f };
    float baseFuelMax{ 300.0f };
};

// Economy & fleet balance (§13.1, §13.4). First-pass, data-driven (§12.6) — cooked
// from an optional `economy { ... }` block; these defaults apply if absent.
struct EconomyTuning
{
    uint16_t fleetCap{ 8 };               // max ships a player commands (§13.1)
    float    cargoCapacity{ 1000.0f };    // per harvester
    float    storageCapacity{ 50000.0f }; // per base
    float    harvestRate{ 50.0f };        // units/sec into cargo
    float    sensorRangeShip{ 5000.0f };
    float    sensorRangeBase{ 12000.0f };
    float    buildOreCost{ 500.0f };      // the basic-ship recipe (§13.4)
    float    buildIceCost{ 200.0f };
    float    buildSeconds{ 20.0f };
    uint8_t  buildShipType{ 1 };
};

struct UniverseDataset
{
    std::vector<RegionDef>        regions;
    std::vector<BeaconDef>        beacons;
    std::vector<ResourceFieldDef> fields;
    NavTuning                     nav{};
    EconomyTuning                 economy{};

    [[nodiscard]] const RegionDef* FindRegion(std::string_view n) const noexcept
    {
        for (const auto& r : regions) if (r.name == n) return &r;
        return nullptr;
    }
    [[nodiscard]] const BeaconDef* FindBeacon(std::string_view n) const noexcept
    {
        for (const auto& b : beacons) if (b.name == n) return &b;
        return nullptr;
    }
    // Region whose bounds contain a sector (first match; nullptr if none).
    [[nodiscard]] const RegionDef* RegionForSector(const Neuron::Universe::SectorId& s) const noexcept
    {
        for (const auto& r : regions) if (r.bounds.Contains(s)) return &r;
        return nullptr;
    }
};

// --- binary codec (Serde; one definition shared by cook + runtime) ----------

namespace detail
{
    inline void WriteStr(Serde::WriteBuffer& wb, const std::string& s)
    {
        wb.WriteUint16(static_cast<uint16_t>(s.size()));
        if (!s.empty()) wb.WriteBytes(s.data(), s.size());
    }
    inline std::string ReadStr(Serde::ReadBuffer& rb)
    {
        const uint16_t n = rb.ReadUint16();
        std::string s(n, '\0');
        if (n) rb.ReadBytes(s.data(), n);
        return s;
    }
    inline void WritePos(Serde::WriteBuffer& wb, const Neuron::Universe::UniversePos& p)
    {
        wb.WriteInt64(p.x); wb.WriteInt64(p.y); wb.WriteInt64(p.z);
    }
    inline Neuron::Universe::UniversePos ReadPos(Serde::ReadBuffer& rb)
    {
        Neuron::Universe::UniversePos p; p.x = rb.ReadInt64(); p.y = rb.ReadInt64(); p.z = rb.ReadInt64(); return p;
    }
} // namespace detail

[[nodiscard]] inline std::vector<uint8_t> EncodeUniverseDataset(const UniverseDataset& d)
{
    Serde::WriteBuffer wb(1024);

    wb.WriteUint16(static_cast<uint16_t>(d.regions.size()));
    for (const auto& r : d.regions) {
        detail::WriteStr(wb, r.name);
        wb.WriteUint8(static_cast<uint8_t>(r.security));
        wb.WriteInt64(r.bounds.x0); wb.WriteInt64(r.bounds.x1);
        wb.WriteInt64(r.bounds.y0); wb.WriteInt64(r.bounds.y1);
        wb.WriteInt64(r.bounds.z0); wb.WriteInt64(r.bounds.z1);
        wb.WriteFloat(r.yieldMult);
    }

    wb.WriteUint16(static_cast<uint16_t>(d.beacons.size()));
    for (const auto& b : d.beacons) {
        detail::WriteStr(wb, b.name);
        detail::WriteStr(wb, b.region);
        detail::WritePos(wb, b.pos);
        wb.WriteUint8(static_cast<uint8_t>(b.kind));
        wb.WriteUint16(static_cast<uint16_t>(b.links.size()));
        for (const auto& l : b.links) detail::WriteStr(wb, l);
    }

    wb.WriteUint16(static_cast<uint16_t>(d.fields.size()));
    for (const auto& f : d.fields) {
        detail::WriteStr(wb, f.name);
        detail::WriteStr(wb, f.region);
        detail::WritePos(wb, f.center);
        wb.WriteFloat(f.radius);
        wb.WriteUint8(static_cast<uint8_t>(f.nodes.size()));
        for (const auto& n : f.nodes) { wb.WriteUint8(static_cast<uint8_t>(n.type)); wb.WriteFloat(n.weight); }
        wb.WriteUint16(f.countMin); wb.WriteUint16(f.countMax);
        wb.WriteUint32(static_cast<uint32_t>(f.yieldMin));
        wb.WriteUint32(static_cast<uint32_t>(f.yieldMax));
        wb.WriteUint32(f.respawnSeconds);
    }

    // Navigation tuning (§13.12).
    wb.WriteFloat(d.nav.warpSpeedShip);     wb.WriteFloat(d.nav.warpSpeedBase);
    wb.WriteFloat(d.nav.warpAlignSeconds);  wb.WriteFloat(d.nav.jumpFuelShip);
    wb.WriteFloat(d.nav.jumpFuelBase);      wb.WriteFloat(d.nav.jumpSpoolShip);
    wb.WriteFloat(d.nav.jumpSpoolBase);     wb.WriteFloat(d.nav.jumpCooldownSeconds);
    wb.WriteFloat(d.nav.beaconUseRange);    wb.WriteFloat(d.nav.shipFuelMax);
    wb.WriteFloat(d.nav.baseFuelMax);

    // Economy & fleet tuning (§13.4).
    wb.WriteUint16(d.economy.fleetCap);
    wb.WriteFloat(d.economy.cargoCapacity);   wb.WriteFloat(d.economy.storageCapacity);
    wb.WriteFloat(d.economy.harvestRate);     wb.WriteFloat(d.economy.sensorRangeShip);
    wb.WriteFloat(d.economy.sensorRangeBase); wb.WriteFloat(d.economy.buildOreCost);
    wb.WriteFloat(d.economy.buildIceCost);    wb.WriteFloat(d.economy.buildSeconds);
    wb.WriteUint8(d.economy.buildShipType);

    wb.Finalise();
    const auto bytes = wb.Data();
    return { bytes.begin(), bytes.end() };
}

[[nodiscard]] inline std::optional<UniverseDataset> DecodeUniverseDataset(std::span<const uint8_t> body)
{
    Serde::ReadBuffer rb(body);
    if (!rb.IsGood()) return std::nullopt;

    UniverseDataset d;

    const uint16_t regionCount = rb.ReadUint16();
    d.regions.reserve(regionCount);
    for (uint16_t i = 0; i < regionCount; ++i) {
        RegionDef r;
        r.name = detail::ReadStr(rb);
        r.security = static_cast<SecurityTier>(rb.ReadUint8());
        r.bounds.x0 = rb.ReadInt64(); r.bounds.x1 = rb.ReadInt64();
        r.bounds.y0 = rb.ReadInt64(); r.bounds.y1 = rb.ReadInt64();
        r.bounds.z0 = rb.ReadInt64(); r.bounds.z1 = rb.ReadInt64();
        r.yieldMult = rb.ReadFloat();
        d.regions.push_back(std::move(r));
    }

    const uint16_t beaconCount = rb.ReadUint16();
    d.beacons.reserve(beaconCount);
    for (uint16_t i = 0; i < beaconCount; ++i) {
        BeaconDef b;
        b.name = detail::ReadStr(rb);
        b.region = detail::ReadStr(rb);
        b.pos = detail::ReadPos(rb);
        b.kind = static_cast<BeaconKind>(rb.ReadUint8());
        const uint16_t linkCount = rb.ReadUint16();
        b.links.reserve(linkCount);
        for (uint16_t j = 0; j < linkCount; ++j) b.links.push_back(detail::ReadStr(rb));
        d.beacons.push_back(std::move(b));
    }

    const uint16_t fieldCount = rb.ReadUint16();
    d.fields.reserve(fieldCount);
    for (uint16_t i = 0; i < fieldCount; ++i) {
        ResourceFieldDef f;
        f.name = detail::ReadStr(rb);
        f.region = detail::ReadStr(rb);
        f.center = detail::ReadPos(rb);
        f.radius = rb.ReadFloat();
        const uint8_t nodeCount = rb.ReadUint8();
        f.nodes.reserve(nodeCount);
        for (uint8_t j = 0; j < nodeCount; ++j) {
            ResourceWeight w;
            w.type = static_cast<ResourceType>(rb.ReadUint8());
            w.weight = rb.ReadFloat();
            f.nodes.push_back(w);
        }
        f.countMin = rb.ReadUint16(); f.countMax = rb.ReadUint16();
        f.yieldMin = static_cast<int32_t>(rb.ReadUint32());
        f.yieldMax = static_cast<int32_t>(rb.ReadUint32());
        f.respawnSeconds = rb.ReadUint32();
        d.fields.push_back(std::move(f));
    }

    d.nav.warpSpeedShip     = rb.ReadFloat(); d.nav.warpSpeedBase    = rb.ReadFloat();
    d.nav.warpAlignSeconds  = rb.ReadFloat(); d.nav.jumpFuelShip     = rb.ReadFloat();
    d.nav.jumpFuelBase      = rb.ReadFloat(); d.nav.jumpSpoolShip    = rb.ReadFloat();
    d.nav.jumpSpoolBase     = rb.ReadFloat(); d.nav.jumpCooldownSeconds = rb.ReadFloat();
    d.nav.beaconUseRange    = rb.ReadFloat(); d.nav.shipFuelMax      = rb.ReadFloat();
    d.nav.baseFuelMax       = rb.ReadFloat();

    d.economy.fleetCap        = rb.ReadUint16();
    d.economy.cargoCapacity   = rb.ReadFloat(); d.economy.storageCapacity = rb.ReadFloat();
    d.economy.harvestRate     = rb.ReadFloat(); d.economy.sensorRangeShip = rb.ReadFloat();
    d.economy.sensorRangeBase = rb.ReadFloat(); d.economy.buildOreCost    = rb.ReadFloat();
    d.economy.buildIceCost    = rb.ReadFloat(); d.economy.buildSeconds    = rb.ReadFloat();
    d.economy.buildShipType   = rb.ReadUint8();

    if (!rb.IsGood()) return std::nullopt;
    return d;
}

// --- referential integrity (this IS what `datacheck` enforces) --------------
//
// Rules (docs/design/universe-worldgen.md §4):
//   regions  — non-empty + unique names; well-formed bounds; yieldMult ≥ 0.
//   beacons  — non-empty + unique names; region resolves; no self-link; every
//              link resolves AND is reciprocal; claimable only in low/null;
//              the PUBLIC-beacon subgraph is connected (no islands).
//   fields   — non-empty + unique names; region resolves; radius > 0; ≥1 node;
//              weights > 0 and sum ≈ 1.0; countMin ≤ countMax; yieldMin ≤ yieldMax.
//
// Returns true iff `errors` is left empty.
[[nodiscard]] inline bool ValidateUniverseDataset(const UniverseDataset& d,
                                                  std::vector<std::string>& errors)
{
    const size_t before = errors.size();
    auto err = [&](std::string m) { errors.push_back(std::move(m)); };

    // Regions ----------------------------------------------------------------
    std::unordered_set<std::string> regionNames;
    for (const auto& r : d.regions) {
        if (r.name.empty()) err("region with empty name");
        else if (!regionNames.insert(r.name).second) err("duplicate region '" + r.name + "'");
        if (!r.bounds.WellFormed()) err("region '" + r.name + "' has malformed bounds (lo > hi)");
        if (r.yieldMult < 0.0f) err("region '" + r.name + "' has negative yield_mult");
    }

    // Beacons ----------------------------------------------------------------
    std::unordered_map<std::string, const BeaconDef*> beaconByName;
    for (const auto& b : d.beacons) {
        if (b.name.empty()) { err("beacon with empty name"); continue; }
        if (!beaconByName.emplace(b.name, &b).second) err("duplicate beacon '" + b.name + "'");
    }
    for (const auto& b : d.beacons) {
        if (b.name.empty()) continue;
        const RegionDef* region = regionNames.count(b.region) ? d.FindRegion(b.region) : nullptr;
        if (!region) err("beacon '" + b.name + "' references unknown region '" + b.region + "'");
        if (b.kind == BeaconKind::Claimable && region && region->security == SecurityTier::High)
            err("beacon '" + b.name + "' is claimable in high-sec region '" + b.region + "' (claimable only in low/null)");
        for (const auto& l : b.links) {
            if (l == b.name) { err("beacon '" + b.name + "' links to itself"); continue; }
            auto it = beaconByName.find(l);
            if (it == beaconByName.end()) { err("beacon '" + b.name + "' links to unknown beacon '" + l + "'"); continue; }
            const auto& back = it->second->links;
            if (std::find(back.begin(), back.end(), b.name) == back.end())
                err("beacon link '" + b.name + "' → '" + l + "' is not reciprocal");
        }
    }

    // Public-beacon connectivity (BFS over public subgraph) ------------------
    {
        std::vector<const BeaconDef*> pub;
        for (const auto& b : d.beacons) if (!b.name.empty() && b.kind == BeaconKind::Public) pub.push_back(&b);
        if (pub.size() > 1) {
            std::unordered_set<std::string> reached;
            std::vector<const BeaconDef*> stack{ pub.front() };
            reached.insert(pub.front()->name);
            while (!stack.empty()) {
                const BeaconDef* cur = stack.back(); stack.pop_back();
                for (const auto& l : cur->links) {
                    auto it = beaconByName.find(l);
                    if (it == beaconByName.end() || it->second->kind != BeaconKind::Public) continue;
                    if (reached.insert(l).second) stack.push_back(it->second);
                }
            }
            for (const auto* b : pub)
                if (!reached.count(b->name))
                    err("public beacon '" + b->name + "' is unreachable from the public network (island)");
        }
    }

    // Resource fields --------------------------------------------------------
    std::unordered_set<std::string> fieldNames;
    for (const auto& f : d.fields) {
        if (f.name.empty()) err("resource field with empty name");
        else if (!fieldNames.insert(f.name).second) err("duplicate field '" + f.name + "'");
        if (!regionNames.count(f.region)) err("field '" + f.name + "' references unknown region '" + f.region + "'");
        if (f.radius <= 0.0f) err("field '" + f.name + "' has non-positive radius");
        if (f.nodes.empty()) err("field '" + f.name + "' has no node composition");
        float sum = 0.0f;
        for (const auto& n : f.nodes) {
            if (n.weight <= 0.0f) err("field '" + f.name + "' has a non-positive weight for " + std::string(ToString(n.type)));
            sum += n.weight;
        }
        if (!f.nodes.empty() && (sum < 0.999f || sum > 1.001f))
            err("field '" + f.name + "' node weights sum to " + std::to_string(sum) + " (expected ≈ 1.0)");
        if (f.countMin > f.countMax) err("field '" + f.name + "' has countMin > countMax");
        if (f.yieldMin > f.yieldMax) err("field '" + f.name + "' has yieldMin > yieldMax");
    }

    // Navigation tuning (§13.12) ---------------------------------------------
    if (d.nav.warpSpeedShip <= 0.0f || d.nav.warpSpeedBase <= 0.0f) err("nav warp speed must be > 0");
    if (d.nav.beaconUseRange <= 0.0f) err("nav beacon_range must be > 0");
    if (d.nav.shipFuelMax <= 0.0f || d.nav.baseFuelMax <= 0.0f) err("nav fuel_max must be > 0");
    if (d.nav.warpAlignSeconds < 0.0f || d.nav.jumpSpoolShip < 0.0f || d.nav.jumpSpoolBase < 0.0f ||
        d.nav.jumpCooldownSeconds < 0.0f || d.nav.jumpFuelShip < 0.0f || d.nav.jumpFuelBase < 0.0f)
        err("nav timings/costs must be ≥ 0");

    // Economy & fleet tuning (§13.4) ----------------------------------------
    if (d.economy.fleetCap == 0 || d.economy.fleetCap > 64) err("economy fleet_cap must be 1..64");
    if (d.economy.cargoCapacity <= 0.0f || d.economy.storageCapacity <= 0.0f) err("economy capacities must be > 0");
    if (d.economy.harvestRate <= 0.0f) err("economy harvest_rate must be > 0");
    if (d.economy.buildSeconds <= 0.0f) err("economy build_seconds must be > 0");
    if (d.economy.buildOreCost < 0.0f || d.economy.buildIceCost < 0.0f) err("economy build costs must be ≥ 0");
    if (d.economy.sensorRangeShip < 0.0f || d.economy.sensorRangeBase < 0.0f) err("economy sensor ranges must be ≥ 0");

    return errors.size() == before;
}

} // namespace Neuron::Sim
