// Universe data tests (masterplan §12.6) — the cook/check pipeline for the
// region/beacon/field schema (docs/design/universe-worldgen.md §4). Pure logic
// (parser + validator + binary codec), so the Linux runner covers it; mirrors
// what `datacook`/`datacheck` do on real authored files.

#include "../datacook/UniverseSource.h"   // pulls NeuronCore/UniverseData.h
#include "TestRunner.h"

#include <string>

using namespace Neuron::Sim;
using namespace Neuron::Tools;
using namespace ertest;

namespace
{
    // A minimal, fully valid dataset: 2 regions (high/low), a connected public
    // beacon pair + a claimable null... no — keep it self-contained & legal.
    const char* kGood =
        "region HOME { security = high  bounds = -16 16 -16 16 -4 4  yield_mult = 0.6 }\n"
        "region EDGE { security = low   bounds = 17 64 -16 16 -4 4   yield_mult = 1.0 }\n"
        "beacon A { region = HOME  pos = 0 0 0        links = B          kind = public }\n"
        "beacon B { region = HOME  pos = 327680 0 0   links = A C        kind = public }\n"
        "beacon C { region = EDGE  pos = 1310720 0 0  links = B          kind = public }\n"
        "field BELT { region = HOME  center = 1000 0 0  radius = 1500\n"
        "            nodes = Ore:0.6 Ice:0.4  count = 4 10  yield = 3000 6000  respawn = 600 }\n";

    UniverseDataset ParseOk(const char* src)
    {
        UniverseDataset ds;
        std::vector<std::string> errs;
        const bool ok = ParseUniverseSource(src, ds, errs);
        ER_CHECK(ok && errs.empty());
        return ds;
    }
}

ER_TEST(UniverseData, ParsesCountsAndFields)
{
    UniverseDataset ds = ParseOk(kGood);
    ER_CHECK_EQ(ds.regions.size(), size_t(2));
    ER_CHECK_EQ(ds.beacons.size(), size_t(3));
    ER_CHECK_EQ(ds.fields.size(), size_t(1));

    const RegionDef* home = ds.FindRegion("HOME");
    ER_CHECK(home != nullptr);
    ER_CHECK(home->security == SecurityTier::High);
    ER_CHECK(home->bounds.x0 == -16 && home->bounds.x1 == 16);

    const BeaconDef* b = ds.FindBeacon("B");
    ER_CHECK(b != nullptr);
    ER_CHECK_EQ(b->links.size(), size_t(2));
    ER_CHECK(b->pos.x == 327680);

    ER_CHECK(ds.fields[0].nodes.size() == 2);
    ER_CHECK(ds.fields[0].countMin == 4 && ds.fields[0].countMax == 10);
}

ER_TEST(UniverseData, ValidGraphPasses)
{
    UniverseDataset ds = ParseOk(kGood);
    std::vector<std::string> errs;
    ER_CHECK(ValidateUniverseDataset(ds, errs));
    ER_CHECK(errs.empty());
}

ER_TEST(UniverseData, BinaryRoundTrip)
{
    UniverseDataset ds = ParseOk(kGood);
    const auto bytes = EncodeUniverseDataset(ds);
    auto rt = DecodeUniverseDataset(bytes);
    ER_CHECK(rt.has_value());
    // Re-encoding the decoded copy must be byte-identical (stable round-trip).
    const auto bytes2 = EncodeUniverseDataset(*rt);
    ER_CHECK(bytes == bytes2);
    // Spot-check a few decoded values survived.
    ER_CHECK_EQ(rt->beacons.size(), size_t(3));
    ER_CHECK(rt->FindBeacon("C") != nullptr);
    ER_CHECK(rt->FindBeacon("C")->region == "EDGE");
    ER_CHECK(rt->beacons[1].pos.x == 327680);
}

ER_TEST(UniverseData, RejectsUnknownRegionRef)
{
    auto ds = ParseOk(
        "region HOME { security = high bounds = 0 4 0 4 0 4 yield_mult = 1 }\n"
        "beacon A { region = NOPE pos = 0 0 0 links = kind = public }\n");
    std::vector<std::string> errs;
    ER_CHECK(!ValidateUniverseDataset(ds, errs));
    ER_CHECK(!errs.empty());
}

ER_TEST(UniverseData, RejectsNonReciprocalLink)
{
    // A → B, but B does not link back to A.
    auto ds = ParseOk(
        "region R { security = low bounds = 0 9 0 9 0 9 yield_mult = 1 }\n"
        "beacon A { region = R pos = 0 0 0 links = B kind = public }\n"
        "beacon B { region = R pos = 1 0 0 links =   kind = public }\n");
    std::vector<std::string> errs;
    ER_CHECK(!ValidateUniverseDataset(ds, errs));
}

ER_TEST(UniverseData, RejectsClaimableInHighSec)
{
    auto ds = ParseOk(
        "region HOME { security = high bounds = 0 9 0 9 0 9 yield_mult = 1 }\n"
        "beacon A { region = HOME pos = 0 0 0 links = kind = claimable }\n");
    std::vector<std::string> errs;
    ER_CHECK(!ValidateUniverseDataset(ds, errs));
}

ER_TEST(UniverseData, RejectsDisconnectedPublicIsland)
{
    // A-B connected; C is a lone public island.
    auto ds = ParseOk(
        "region R { security = low bounds = 0 99 0 9 0 9 yield_mult = 1 }\n"
        "beacon A { region = R pos = 0 0 0 links = B kind = public }\n"
        "beacon B { region = R pos = 1 0 0 links = A kind = public }\n"
        "beacon C { region = R pos = 2 0 0 links =   kind = public }\n");
    std::vector<std::string> errs;
    ER_CHECK(!ValidateUniverseDataset(ds, errs));
}

ER_TEST(UniverseData, RejectsBadNodeWeights)
{
    // Weights sum to 0.5, not ~1.0.
    auto ds = ParseOk(
        "region R { security = low bounds = 0 9 0 9 0 9 yield_mult = 1 }\n"
        "field F { region = R center = 0 0 0 radius = 100 nodes = Ore:0.3 Ice:0.2"
        " count = 1 2 yield = 1 2 respawn = 1 }\n");
    std::vector<std::string> errs;
    ER_CHECK(!ValidateUniverseDataset(ds, errs));
}

ER_TEST(UniverseData, ReportsSyntaxErrorWithLine)
{
    UniverseDataset ds;
    std::vector<std::string> errs;
    const bool ok = ParseUniverseSource(
        "region R { security = high bounds = 0 4 0 4 0 4 yield_mult = 1 }\n"
        "beacon A { bogus = 3 }\n", ds, errs);
    ER_CHECK(!ok);
    ER_CHECK(!errs.empty());
    // Error should mention the offending line (line 2).
    ER_CHECK(errs[0].find("line 2") != std::string::npos);
}

ER_TEST(UniverseData, ParsesTuningBlockAndKeepsDefaults)
{
    auto ds = ParseOk(
        "region R { security = low bounds = 0 9 0 9 0 9 yield_mult = 1 }\n"
        "tuning { warp_speed_ship = 7000  jump_fuel_base = 50  base_fuel_max = 250 }\n");
    ER_CHECK(ds.nav.warpSpeedShip == 7000.0f);
    ER_CHECK(ds.nav.jumpFuelBase == 50.0f);
    ER_CHECK(ds.nav.baseFuelMax == 250.0f);
    ER_CHECK(ds.nav.warpSpeedBase == 2000.0f); // untouched key keeps its default
    std::vector<std::string> errs;
    ER_CHECK(ValidateUniverseDataset(ds, errs));
    // The tuning survives a binary round-trip.
    auto rt = DecodeUniverseDataset(EncodeUniverseDataset(ds));
    ER_CHECK(rt.has_value() && rt->nav.warpSpeedShip == 7000.0f && rt->nav.baseFuelMax == 250.0f);
}

ER_TEST(UniverseData, RejectsBadTuning)
{
    auto ds = ParseOk(
        "region R { security = low bounds = 0 9 0 9 0 9 yield_mult = 1 }\n"
        "tuning { warp_speed_ship = 0 }\n"); // warp speed must be > 0
    std::vector<std::string> errs;
    ER_CHECK(!ValidateUniverseDataset(ds, errs));
}
