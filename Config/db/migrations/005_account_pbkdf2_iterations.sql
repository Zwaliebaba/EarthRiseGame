-- Migration 005 — store the PBKDF2 iteration count per account
-- Forward migration from 004. Implements masterplan §14 ("high + tunable iteration
-- count, stored per hash, so the cost can be raised later") — the M5 plan area C
-- requirement. AccountStore writes this on register and verifies Login with the
-- account's STORED cost, so raising the global default never locks out existing users.
-- Idempotent (guarded by sys.columns). Azure-SQL-compatible.
--
-- The default (210000) matches NeuronCore-side kDefaultPbkdf2Iterations (OWASP-tier
-- first pass for PBKDF2-HMAC-SHA512), so any pre-005 account — which was hashed with
-- that same configured constant — verifies correctly after this back-fill.

IF NOT EXISTS (SELECT 1 FROM sys.columns
               WHERE object_id = OBJECT_ID('Accounts') AND name = 'Pbkdf2Iterations')
BEGIN
    ALTER TABLE Accounts ADD Pbkdf2Iterations INT NOT NULL
        CONSTRAINT DF_Accounts_Pbkdf2Iterations DEFAULT 210000;
END
GO
