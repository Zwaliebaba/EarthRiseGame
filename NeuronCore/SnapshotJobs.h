#pragma once
// SnapshotJobs.h — snapshot job-pool partitioning + determinism seam (M4 area F, §9).
//
// Per-client snapshot encoding (areas C–E) is, with sim time, the first thing to
// blow the 33.3 ms budget at hundreds of players, so it runs over a read-only job
// pool against a frozen post-tick state: each worker encodes its slice of clients,
// the results are gathered for the net layer to seal/send. Because encode is a pure
// projection of post-tick state (versions/positions/interest don't change during the
// pass), the gathered output is identical no matter how the clients are partitioned
// across workers — that order-independence is the determinism gate (§16.1).
//
// This header owns the platform-independent partitioning + the gather/merge, and a
// serial reference encoder the test compares against. The actual OS thread pool and
// the frozen read-view isolation (no concurrent sim writes during encode; per-client
// baseline entries pre-created so workers touch disjoint state) are the Win32 ERServer
// side. Mirrored on the Linux testrunner (§16.2).

#include "ServerUniverse.h"
#include "Snapshot.h"

#include <cstdint>
#include <vector>

namespace Neuron::Sim
{

// One client's sealed snapshot from the pool.
struct EncodeResult
{
    uint32_t      clientId{ 0 };
    DeltaSnapshot snap;
};

// Round-robin 'clients' across 'workerCount' partitions — each client in exactly one
// partition, every partition encoded by one worker. Deterministic and disjoint
// (workers never share a client's per-connection state). workerCount 0 ⇒ 1.
[[nodiscard]] inline std::vector<std::vector<uint32_t>>
PartitionClients(const std::vector<uint32_t>& clients, size_t workerCount)
{
    const size_t w = workerCount == 0 ? 1 : workerCount;
    std::vector<std::vector<uint32_t>> parts(w);
    for (size_t i = 0; i < clients.size(); ++i) parts[i % w].push_back(clients[i]);
    return parts;
}

// Serial reference: encode every client's snapshot in client order (the single-
// threaded baseline the pooled path must match byte-for-byte). 'capped' (optional)
// accumulates the R16 cap-bind total across clients (area I evidence).
[[nodiscard]] inline std::vector<EncodeResult>
EncodeClientsSerial(ServerUniverse& su, const std::vector<uint32_t>& clients,
                    size_t byteBudget, size_t* totalCapped = nullptr)
{
    std::vector<EncodeResult> out;
    out.reserve(clients.size());
    size_t capSum = 0;
    for (uint32_t c : clients) {
        size_t capped = 0;
        out.push_back({ c, su.BuildClientSnapshot(c, byteBudget, &capped) });
        capSum += capped;
    }
    if (totalCapped) *totalCapped = capSum;
    return out;
}

// Pooled encode modelled as the job pool would run it: partition the clients across
// 'workerCount', encode each partition, then gather the results back into client
// order. The executor here is serial (the testrunner is deterministic); ERServer
// injects real threads. The gathered output equals EncodeClientsSerial — the
// determinism property the pool relies on.
[[nodiscard]] inline std::vector<EncodeResult>
EncodeClientsPooled(ServerUniverse& su, const std::vector<uint32_t>& clients,
                    size_t byteBudget, size_t workerCount)
{
    const auto parts = PartitionClients(clients, workerCount);
    std::vector<std::vector<EncodeResult>> perWorker(parts.size());
    for (size_t w = 0; w < parts.size(); ++w)       // ← each iteration is one worker
        perWorker[w] = EncodeClientsSerial(su, parts[w], byteBudget);

    // Gather in the original client order so the seal/send layer is partition-agnostic.
    std::vector<EncodeResult> out;
    out.reserve(clients.size());
    std::vector<size_t> cursor(parts.size(), 0);
    for (size_t i = 0; i < clients.size(); ++i) {
        const size_t w = i % parts.size();
        out.push_back(perWorker[w][cursor[w]++]);
    }
    return out;
}

} // namespace Neuron::Sim
