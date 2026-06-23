// M5: Windows/ODBC integration — unverified on Linux (no MSBuild/ODBC/SQL here); validate on the Windows build agent against the dev SQL Server.
//
// ERServerTest.cpp — unit tests for the M5 persistence layer (areas A/C/D/E/F).
//
// What runs here WITHOUT a live SQL Server (the §16.1 unit tier; the DB round-trips
// are the §16.3 integration tier on the dev SQL instance):
//   * PBKDF2 cross-check  — CngCrypto::Pbkdf2HmacSha512 (BCryptDeriveKeyPBKDF2) MUST be
//     byte-identical to the FIPS/RFC-verified portable reference
//     Neuron::Crypto::Pbkdf2HmacSha512 (this is how the agent validates CNG == ref).
//   * OdbcConnection::BuildAuthFragment — SQL-login vs managed-identity vs Entra string
//     selection (area A/J: the managed-identity path must EXIST for the Azure migration).
//   * PersistQueue — the zero-loss vs bounded(drop-oldest) hand-off used by the
//     persistence thread (the §9 "SQL never stalls the tick" + §15 RPO asymmetry).
//   * Outbox model — the zero-loss + idempotent-replay contract the SQL EconomyStore
//     mirrors exactly (Outbox.h), so the contract is unit-tested, not just asserted vs a DB.
//   * WarmRestart — the snapshot blob encode/decode/hash SimSnapshotStore persists (area F).
//   * ReconnectPolicy — backoff/jitter anti-herd schedule (area G mirror).

#include "pch.h"
#include "CppUnitTest.h"

// Production CNG crypto (compiled into this project) + the portable reference.
#include "CngCrypto.h"
#include "Pbkdf2.h"

// Pure persist models / header-only pieces.
#include "Outbox.h"
#include "WarmRestart.h"
#include "Reconnect.h"

// Persist-layer headers (OdbcConnection.cpp is compiled in for BuildAuthFragment;
// PersistQueue.h is header-only).
#include "persist/OdbcConnection.h"
#include "persist/PersistConfig.h"
#include "persist/PersistQueue.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace
{
std::span<const uint8_t> AsBytes(const std::string& s)
{
    return { reinterpret_cast<const uint8_t*>(s.data()), s.size() };
}
} // namespace

namespace ERServerTest
{

// =====================================================================================
// Area C — PBKDF2 CNG vs portable reference (byte-identical KDF)
// =====================================================================================
TEST_CLASS(Pbkdf2CngVsReferenceTests)
{
    // Cross-check CNG against the reference for one (password, salt, iterations) tuple.
    void CrossCheck(const std::string& pw, const std::string& salt, uint32_t iters, size_t dkLen)
    {
        Neuron::Net::CngCrypto crypto;
        Assert::IsTrue(crypto.Initialize(), L"CNG init");

        const std::vector<uint8_t> cng =
            crypto.Pbkdf2HmacSha512(AsBytes(pw), AsBytes(salt), iters, dkLen);
        const std::vector<uint8_t> ref =
            Neuron::Crypto::Pbkdf2HmacSha512(AsBytes(pw), AsBytes(salt), iters, dkLen);

        Assert::AreEqual(dkLen, cng.size(), L"CNG output length");
        Assert::AreEqual(ref.size(), cng.size(), L"length parity");
        for (size_t i = 0; i < dkLen; ++i)
            Assert::AreEqual(ref[i], cng[i], L"PBKDF2 byte mismatch CNG vs reference");
    }

public:
    // Low iteration counts keep the test fast; the byte-identity property is
    // iteration-count-independent, so a few rounds prove the algorithms agree.
    TEST_METHOD(MatchesReference_VariousTuples)
    {
        CrossCheck("password", "salt", 1, 64);
        CrossCheck("password", "salt", 2, 64);
        CrossCheck("passwordPASSWORDpassword", "saltSALTsaltSALT", 16, 64);
        CrossCheck("", "salt-only", 8, 64);
        CrossCheck("p", "", 8, 64);                 // empty salt path
        CrossCheck("hunter2-with-pepper-appended", "0123456789abcdef0123456789abcdef", 32, 64);
    }

    TEST_METHOD(MatchesReference_NonBlockMultipleLength)
    {
        // dkLen not a multiple of the 64-byte SHA-512 block exercises the final-block
        // truncation path on both implementations.
        CrossCheck("password", "salt", 4, 50);
        CrossCheck("password", "salt", 4, 100);
    }

    TEST_METHOD(DifferentSaltYieldsDifferentHash)
    {
        Neuron::Net::CngCrypto crypto;
        Assert::IsTrue(crypto.Initialize());
        const auto a = crypto.Pbkdf2HmacSha512(AsBytes(std::string("pw")), AsBytes(std::string("saltA")), 8, 64);
        const auto b = crypto.Pbkdf2HmacSha512(AsBytes(std::string("pw")), AsBytes(std::string("saltB")), 8, 64);
        Assert::IsFalse(a == b, L"distinct salts must give distinct hashes");
    }
};

// =====================================================================================
// Area A/J — ODBC connection-string auth selection (SQL login vs managed identity)
// =====================================================================================
TEST_CLASS(OdbcAuthFragmentTests)
{
public:
    TEST_METHOD(SqlLogin_EmbedsCredentials)
    {
        Neuron::Persist::PersistConfig cfg;
        cfg.authMode = Neuron::Persist::DbAuthMode::SqlLogin;
        cfg.sqlUser = "svc_login";
        cfg.sqlPassword = "S3cret!";
        const std::string frag = Neuron::Persist::OdbcConnection::BuildAuthFragment(cfg);
        Assert::IsTrue(frag.find("UID=svc_login") != std::string::npos, L"UID present");
        Assert::IsTrue(frag.find("PWD=S3cret!") != std::string::npos, L"PWD present");
        Assert::IsTrue(frag.find("ActiveDirectory") == std::string::npos, L"no AD auth for SQL login");
    }

    TEST_METHOD(ManagedIdentity_UsesActiveDirectoryMsi_NoSecret)
    {
        // area J: the Azure SQL migration is a config flip — the managed-identity path
        // must exist and carry NO secret in the connection string.
        Neuron::Persist::PersistConfig cfg;
        cfg.authMode = Neuron::Persist::DbAuthMode::ManagedIdentity;
        cfg.sqlUser = "ignored";
        cfg.sqlPassword = "ignored";
        const std::string frag = Neuron::Persist::OdbcConnection::BuildAuthFragment(cfg);
        Assert::IsTrue(frag.find("Authentication=ActiveDirectoryMsi") != std::string::npos,
                       L"managed identity uses ActiveDirectoryMsi");
        Assert::IsTrue(frag.find("PWD=") == std::string::npos, L"no password in MSI string");
    }

    TEST_METHOD(EntraPassword_UsesActiveDirectoryPassword)
    {
        Neuron::Persist::PersistConfig cfg;
        cfg.authMode = Neuron::Persist::DbAuthMode::EntraPassword;
        cfg.sqlUser = "user@tenant";
        cfg.sqlPassword = "pw";
        const std::string frag = Neuron::Persist::OdbcConnection::BuildAuthFragment(cfg);
        Assert::IsTrue(frag.find("Authentication=ActiveDirectoryPassword") != std::string::npos);
        Assert::IsTrue(frag.find("UID=user@tenant") != std::string::npos);
    }
};

// =====================================================================================
// Area A/D/E — the MPSC hand-off (SQL never stalls the tick; RPO asymmetry)
// =====================================================================================
TEST_CLASS(PersistQueueTests)
{
public:
    TEST_METHOD(ZeroLossQueue_PreservesOrderAndNeverDrops)
    {
        Neuron::Persist::MpscZeroLossQueue<int> q;
        for (int i = 0; i < 1000; ++i)
            q.Push(int{ i });
        Assert::AreEqual(size_t{ 1000 }, q.Depth(), L"economy queue never drops (zero-loss)");

        std::vector<int> out;
        q.DrainInto(out);
        Assert::AreEqual(size_t{ 1000 }, out.size());
        for (int i = 0; i < 1000; ++i)
            Assert::AreEqual(i, out[static_cast<size_t>(i)], L"FIFO order preserved (ordered drain)");
        Assert::AreEqual(size_t{ 0 }, q.Depth(), L"drained empty");
    }

    TEST_METHOD(BoundedQueue_DropsOldestPastCapacity)
    {
        // Bounded RPO (§15): the write-behind queue caps and drops OLDEST under backlog.
        Neuron::Persist::MpscBoundedQueue<int> q(4);
        for (int i = 0; i < 4; ++i)
            Assert::IsTrue(q.Push(int{ i }), L"within capacity accepts");
        // 5th push must drop the oldest (0) and report the drop.
        Assert::IsFalse(q.Push(int{ 4 }), L"over capacity drops-oldest");
        Assert::AreEqual(uint64_t{ 1 }, q.Dropped());

        std::vector<int> out;
        q.DrainInto(out);
        Assert::AreEqual(size_t{ 4 }, out.size());
        Assert::AreEqual(1, out.front(), L"oldest (0) was dropped; window starts at 1");
        Assert::AreEqual(4, out.back());
    }
};

// =====================================================================================
// Area D — the zero-loss + idempotent replay contract EconomyStore mirrors (Outbox.h)
// =====================================================================================
TEST_CLASS(EconomyOutboxContractTests)
{
public:
    // This pins the SEMANTICS the SQL EconomyStore implements (IdempotencyKey UNIQUE
    // index ⇒ exactly-once). If this contract changes, EconomyStore must change with it.
    TEST_METHOD(CrashBeforeDrain_ReplaysWithZeroLossAndNoDoubleCredit)
    {
        using namespace Neuron::Persist;
        Outbox outbox;
        EconomyState state;

        // Three economy events appended (the same-transaction outbox stage).
        outbox.Append(/*idem*/ 1001, /*acct*/ 7, /*amount*/ +500);
        outbox.Append(/*idem*/ 1002, /*acct*/ 7, /*amount*/ -200);
        outbox.Append(/*idem*/ 1003, /*acct*/ 9, /*amount*/ +50);

        // First drain applies all three.
        const size_t applied = outbox.Drain(state);
        Assert::AreEqual(size_t{ 3 }, applied);
        Assert::AreEqual(int64_t{ 300 }, state.Balance(7));
        Assert::AreEqual(int64_t{ 50 }, state.Balance(9));
        outbox.ConfirmDrained(outbox.MaxSeq());

        // Simulate a crash that LOST the in-memory drain watermark but not the durably
        // committed wallet/ledger (the idemKey dedupe survives in the transaction).
        state.DropWatermarkOnly();

        // Full replay (the warm-restart path) must NOT double-apply — idemKey dedupes.
        const size_t reapplied = outbox.ReplayAll(state);
        Assert::AreEqual(size_t{ 0 }, reapplied, L"replay applies nothing new (idempotent)");
        Assert::AreEqual(int64_t{ 300 }, state.Balance(7), L"no double-credit after replay");
        Assert::AreEqual(int64_t{ 50 }, state.Balance(9));
        Assert::AreEqual(size_t{ 3 }, state.LedgerSize(), L"ledger has exactly 3 rows");
    }

    TEST_METHOD(ForcedFailure_RollsBackAtomically)
    {
        using namespace Neuron::Persist;
        EconomyState state;
        EconomyEvent e{ /*seq*/ 1, /*idem*/ 42, /*acct*/ 3, /*amount*/ +100 };
        Assert::IsFalse(state.Apply(e, /*injectFailure*/ true), L"forced failure applies nothing");
        Assert::AreEqual(int64_t{ 0 }, state.Balance(3), L"balance unchanged on rollback");
        Assert::AreEqual(size_t{ 0 }, state.LedgerSize());
        // The same event then succeeds (the retry the persistence thread performs).
        Assert::IsTrue(state.Apply(e));
        Assert::AreEqual(int64_t{ 100 }, state.Balance(3));
    }
};

// =====================================================================================
// Area F — warm-restart snapshot blob (the bytes SimSnapshotStore persists)
// =====================================================================================
TEST_CLASS(WarmRestartBlobTests)
{
public:
    TEST_METHOD(EncodeDecode_RoundTripsAndHashMatches)
    {
        using namespace Neuron::Persist;
        PersistState s;
        s.tick = 123456;
        s.outboxSeq = 9001; // the watermark SimSnapshots.OutboxWatermark mirrors
        s.bases.push_back(PersistBase{ /*netId*/ 1, /*owner*/ 7, 100, 200, 300,
                                       5000, 5000, 5000, { 10.f, 20.f, 30.f }, 99.f, 1, 0 });
        s.ships.push_back(PersistShip{ 2, 7, 400, 500, 600, 100, { 1.f, 2.f, 3.f }, 5 });
        s.builds.push_back(PersistBuild{ 7, 42, 0.5f });
        s.npcs.push_back(PersistNpc{ 3, 700, 800, 900, 50, 11, 2 });

        const std::vector<uint8_t> blob = EncodeState(s);
        Assert::IsFalse(blob.empty(), L"blob produced");

        PersistState back;
        Assert::IsTrue(DecodeState(blob, back), L"decode ok");
        Assert::AreEqual(StateHash(s), StateHash(back), L"structural hash matches after round-trip");
        Assert::AreEqual(s.outboxSeq, back.outboxSeq, L"outbox watermark survives the blob");
        Assert::AreEqual(size_t{ 1 }, back.bases.size());
        Assert::AreEqual(size_t{ 1 }, back.npcs.size());
    }

    TEST_METHOD(TruncatedBlob_FailsCleanly)
    {
        using namespace Neuron::Persist;
        PersistState s; s.tick = 1; s.bases.push_back(PersistBase{});
        std::vector<uint8_t> blob = EncodeState(s);
        blob.resize(blob.size() / 2); // truncate
        PersistState back;
        Assert::IsFalse(DecodeState(blob, back), L"truncated blob rejected, not crashed");
    }
};

// =====================================================================================
// Area G — reconnect backoff/jitter (anti-herd schedule the client uses on warm restart)
// =====================================================================================
TEST_CLASS(ReconnectPolicyTests)
{
public:
    TEST_METHOD(Ceiling_GrowsExponentiallyAndCaps)
    {
        Neuron::Net::ReconnectPolicy p; // 500ms base, x2, cap 30s
        Assert::AreEqual(uint32_t{ 500 }, p.CeilingMs(0));
        Assert::AreEqual(uint32_t{ 1000 }, p.CeilingMs(1));
        Assert::AreEqual(uint32_t{ 2000 }, p.CeilingMs(2));
        Assert::AreEqual(uint32_t{ 30000 }, p.CeilingMs(20), L"saturates at cap, no overflow");
    }

    TEST_METHOD(FullJitter_StaysWithinCeiling_AndFleetSpreads)
    {
        Neuron::Net::ReconnectPolicy p;
        // Full jitter ⇒ delay in [0, ceiling].
        Assert::AreEqual(uint32_t{ 0 }, p.DelayMs(3, 0.0));
        Assert::IsTrue(p.DelayMs(3, 0.999999) <= p.CeilingMs(3));

        // A fleet seeded per-id must not synchronise (anti-herd, R22): collect distinct
        // delays at the same attempt and assert real spread.
        std::vector<uint32_t> delays;
        for (uint64_t id = 0; id < 32; ++id) {
            Neuron::Net::JitterRng rng(id);
            delays.push_back(p.DelayMs(4, rng.Next01()));
        }
        uint32_t mn = delays.front(), mx = delays.front();
        for (uint32_t d : delays) { mn = (d < mn ? d : mn); mx = (d > mx ? d : mx); }
        Assert::IsTrue(mx - mn > p.CeilingMs(4) / 4, L"reconnect attempts spread across the window");
    }
};

} // namespace ERServerTest
