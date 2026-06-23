-- Migration 004 — M5 durability hardening: exactly-once outbox + snapshot watermark
-- Forward migration from 003. Implements the masterplan §15 / §17-M5 "zero economy
-- loss" + idempotent warm-restart replay contract, matching the verified portable
-- model (NeuronCore/Outbox.h, NeuronCore/WarmRestart.h; testrunner OutboxTests /
-- WarmRestartTests, §16.2). Idempotent (guarded by sys.columns). Azure-SQL-compatible
-- (no cross-DB / SQL Agent / FILESTREAM, §15).
--
-- Why:
--  1. EconomyOutbox had no idempotency key — a crash mid-drain (or a lost drain
--     watermark) could re-apply an event and double-credit. An IdempotencyKey with a
--     UNIQUE index makes the drain exactly-once: a replayed event collides and is
--     skipped (the OutboxTests "CrashBeforeDrainReplaysWithZeroLossAndNoDoubleCredit"
--     invariant, enforced in SQL).
--  2. SimSnapshots had no outbox watermark — "replay EconomyOutbox since the snapshot"
--     was imprecise. OutboxWatermark records the max OutboxId already reflected in the
--     blob, so restart replays exactly `WHERE OutboxId > OutboxWatermark`
--     (WarmRestartTests "SnapshotPlusLogEqualsContinuousRun").

-- ============================================================
-- 1. EconomyOutbox.IdempotencyKey — exactly-once dedupe for ordered drain/replay
-- ============================================================
IF NOT EXISTS (SELECT 1 FROM sys.columns
               WHERE object_id = OBJECT_ID('EconomyOutbox') AND name = 'IdempotencyKey')
BEGIN
    -- Nullable add (no rewrite of any existing rows); new writes always set it.
    ALTER TABLE EconomyOutbox ADD IdempotencyKey BIGINT NULL;
END
GO

-- UNIQUE only where set (filtered index — Azure-SQL-compatible): a re-staged event
-- with the same key is rejected, so replay can never insert a duplicate effect.
IF NOT EXISTS (SELECT 1 FROM sys.indexes WHERE name = 'UX_EconomyOutbox_IdemKey')
BEGIN
    EXEC('CREATE UNIQUE INDEX UX_EconomyOutbox_IdemKey ON EconomyOutbox (IdempotencyKey)
          WHERE IdempotencyKey IS NOT NULL;');
END
GO

-- ============================================================
-- 2. SimSnapshots.OutboxWatermark — precise replay-since-snapshot boundary
-- The blob already carries this watermark internally (WarmRestart PersistState
-- .outboxSeq); the column surfaces it for the restart query without parsing the blob.
-- ============================================================
IF NOT EXISTS (SELECT 1 FROM sys.columns
               WHERE object_id = OBJECT_ID('SimSnapshots') AND name = 'OutboxWatermark')
BEGIN
    ALTER TABLE SimSnapshots ADD OutboxWatermark BIGINT NOT NULL DEFAULT 0;
END
GO

-- ============================================================
-- 3. CurrencyLedger: dedupe the same economy event landing twice (defence in depth).
-- The ledger is the audit trail behind the wallet; a UNIQUE (RefType, RefId) where
-- both are set stops a replayed Build/Kill/Trade from appending a second ledger row.
-- ============================================================
IF NOT EXISTS (SELECT 1 FROM sys.indexes WHERE name = 'UX_CurrencyLedger_Ref')
BEGIN
    EXEC('CREATE UNIQUE INDEX UX_CurrencyLedger_Ref ON CurrencyLedger (RefType, RefId)
          WHERE RefType IS NOT NULL AND RefId IS NOT NULL;');
END
GO
