// Write-through economy outbox tests (M5 area D; §15, §17 M5 "zero economy loss").
// Every economy mutation appends an ordered, durable event committed with the
// balance change; the drain applies in order and is idempotent, so a crash
// mid-drain (or a lost watermark) replays with zero loss and no double-credit.
// Pure model; mirrored on the Linux runner (§16.2). The ODBC writes are Win32/SQL.

#include "Outbox.h"
#include "TestRunner.h"

using namespace ertest;
using Neuron::Persist::EconomyEvent;
using Neuron::Persist::EconomyEventType;
using Neuron::Persist::EconomyState;
using Neuron::Persist::Outbox;

ER_TEST(Outbox, ApplyUpdatesWalletAndLedgerAtomically)
{
    EconomyState s;
    EconomyEvent e{ 1, 0xA001, 42, 1000, EconomyEventType::WalletDelta };

    // Injected failure rolls the whole apply back: neither wallet nor ledger move.
    ER_CHECK(!s.Apply(e, /*injectFailure=*/true));
    ER_CHECK_EQ(s.Balance(42), int64_t{ 0 });
    ER_CHECK_EQ(s.LedgerSize(), size_t{ 0 });

    // A clean apply moves both together (balance + one ledger row).
    ER_CHECK(s.Apply(e));
    ER_CHECK_EQ(s.Balance(42), int64_t{ 1000 });
    ER_CHECK_EQ(s.LedgerSize(), size_t{ 1 });
    ER_CHECK_EQ(s.Ledger().back().balanceAfter, int64_t{ 1000 });
}

ER_TEST(Outbox, ApplyIsIdempotentByIdemKey)
{
    EconomyState s;
    EconomyEvent e{ 7, 0xBEEF, 1, 500, EconomyEventType::WalletDelta };
    ER_CHECK(s.Apply(e));        // first time: applied
    ER_CHECK(!s.Apply(e));       // same idemKey: no-op (no double-credit)
    ER_CHECK_EQ(s.Balance(1), int64_t{ 500 });
    ER_CHECK_EQ(s.LedgerSize(), size_t{ 1 });
}

ER_TEST(Outbox, OrderedDrainAppliesAndAdvancesWatermark)
{
    Outbox ob;
    ob.Append(0x1, 1, 100);
    ob.Append(0x2, 1, 200);
    ob.Append(0x3, 2, 50);
    ER_CHECK_EQ(ob.PendingCount(), size_t{ 3 });

    EconomyState s;
    ER_CHECK_EQ(ob.Drain(s), size_t{ 3 });
    ob.ConfirmDrained(ob.MaxSeq());
    ER_CHECK_EQ(ob.PendingCount(), size_t{ 0 });
    ER_CHECK_EQ(s.Balance(1), int64_t{ 300 });
    ER_CHECK_EQ(s.Balance(2), int64_t{ 50 });

    // A fresh event after a confirmed drain is the only thing pending.
    ob.Append(0x4, 1, 7);
    ER_CHECK_EQ(ob.PendingCount(), size_t{ 1 });
    ER_CHECK_EQ(ob.Drain(s), size_t{ 1 });
    ER_CHECK_EQ(s.Balance(1), int64_t{ 307 });
}

ER_TEST(Outbox, CrashBeforeDrainReplaysWithZeroLossAndNoDoubleCredit)
{
    // Producer stages N economy events durably.
    Outbox ob;
    for (uint64_t i = 1; i <= 6; ++i) ob.Append(0x1000 + i, 1, static_cast<int64_t>(i) * 10);
    const int64_t expected = (1 + 2 + 3 + 4 + 5 + 6) * 10; // 210

    // Reference: a clean, fully confirmed drain.
    EconomyState ref;
    ob.Drain(ref);
    ER_CHECK_EQ(ref.Balance(1), expected);
    ER_CHECK_EQ(ref.LedgerSize(), size_t{ 6 });

    // Crash mid-drain: half applied, the drain watermark never confirmed.
    EconomyState crashed;
    for (int i = 0; i < 3; ++i) crashed.Apply(ob.Log()[i]); // first 3 committed before crash
    // Restart: the watermark is gone but wallet/ledger/dedupe survived (one txn).
    crashed.DropWatermarkOnly();
    const size_t newlyApplied = ob.ReplayAll(crashed);
    ER_CHECK_EQ(newlyApplied, size_t{ 3 });           // only the 3 un-applied ones
    ER_CHECK_EQ(crashed.Balance(1), expected);        // zero loss
    ER_CHECK_EQ(crashed.LedgerSize(), size_t{ 6 });   // no double-credit (idemKey dedupe)

    // A second replay (a second restart) is a complete no-op.
    ER_CHECK_EQ(ob.ReplayAll(crashed), size_t{ 0 });
    ER_CHECK_EQ(crashed.Balance(1), expected);
}
