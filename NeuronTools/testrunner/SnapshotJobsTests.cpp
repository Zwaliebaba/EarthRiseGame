// Snapshot job-pool determinism tests (masterplan §9; M4 area F). Per-client encode
// runs over a read-only job pool against frozen post-tick state; because encode is a
// pure projection, the gathered output is byte-identical no matter how the clients
// are partitioned across workers. That order-independence is the gate (§16.1). Pure;
// mirrored on the Linux runner (§16.2). The OS thread pool is the Win32 ERServer side.
//
// Component ids are bound once per binary in ShapeCatalogTests.cpp.

#include "ServerUniverse.h"
#include "Snapshot.h"
#include "SnapshotJobs.h"
#include "TestRunner.h"

#include <algorithm>
#include <vector>

using namespace ertest;
using namespace Neuron::Sim;
using Neuron::Universe::UniversePos;

namespace
{
    // A scene with 'players' bases spread one sector apart, each with nearby props.
    std::vector<uint32_t> SeedClients(ServerUniverse& su, int players)
    {
        std::vector<uint32_t> clients;
        for (int p = 0; p < players; ++p) {
            const int64_t bx = static_cast<int64_t>(p) * Neuron::Universe::SECTOR_SIZE;
            const uint32_t base = su.SpawnBase({ bx, 0, 0 }, { 0, 0, 0 });
            clients.push_back(base);
            for (int i = 0; i < 3; ++i) su.SpawnProp(0, { bx + 100 + 10 * i, 0, 0 });
        }
        su.Step(0.1f); // stamp + interest
        return clients;
    }

    // Concatenated encoded bytes of a gathered result set (the wire output).
    std::vector<uint8_t> Wire(const std::vector<EncodeResult>& results)
    {
        std::vector<uint8_t> all;
        for (const auto& r : results) {
            const auto b = EncodeDeltaSnapshot(r.snap);
            all.push_back(static_cast<uint8_t>(r.clientId & 0xFF));
            all.insert(all.end(), b.begin(), b.end());
        }
        return all;
    }
}

ER_TEST(SnapshotJobs, PartitionCoversEveryClientOnce)
{
    std::vector<uint32_t> clients = { 1, 2, 3, 4, 5, 6, 7 };
    const auto parts = PartitionClients(clients, 3);
    ER_CHECK_EQ(parts.size(), size_t{ 3 });

    std::vector<uint32_t> seen;
    for (const auto& p : parts)
        for (uint32_t c : p) seen.push_back(c);
    std::sort(seen.begin(), seen.end());
    ER_CHECK((seen == std::vector<uint32_t>{ 1, 2, 3, 4, 5, 6, 7 })); // each exactly once
}

ER_TEST(SnapshotJobs, PooledEncodeMatchesSerialReferenceForAnyWorkerCount)
{
    ServerUniverse su(false);
    const std::vector<uint32_t> clients = SeedClients(su, 8);

    // The single-threaded reference (encode is idempotent without an intervening
    // ack/record, so re-encoding the same frozen state reproduces the same bytes).
    const std::vector<uint8_t> reference = Wire(EncodeClientsSerial(su, clients, 256));

    for (size_t workers : { size_t{ 1 }, size_t{ 2 }, size_t{ 3 }, size_t{ 5 }, size_t{ 8 }, size_t{ 16 } }) {
        const std::vector<uint8_t> pooled = Wire(EncodeClientsPooled(su, clients, 256, workers));
        ER_CHECK(pooled == reference); // byte-identical regardless of partitioning
    }
}

ER_TEST(SnapshotJobs, PooledGatherPreservesClientOrder)
{
    ServerUniverse su(false);
    const std::vector<uint32_t> clients = SeedClients(su, 6);
    const auto pooled = EncodeClientsPooled(su, clients, 256, 4);
    ER_CHECK_EQ(pooled.size(), clients.size());
    for (size_t i = 0; i < clients.size(); ++i)
        ER_CHECK_EQ(pooled[i].clientId, clients[i]); // gathered back into client order
}
