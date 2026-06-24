-- EarthRise — dev account seed
-- =============================================================================
-- Creates ONE game account whose username + password match the dev credentials
-- the client ships with (EarthRise/App.cpp kDevAuthUser / kDevAuthPassword, and
-- ERHeadless bot0..), so you can log in immediately without the client having to
-- auto-register on first connect.
--
--   Username : player1
--   Password : player1-devpass
--
-- This is a GAME account row (the §14 Accounts table the auth layer reads), NOT a
-- SQL Server login — the DB connection login (database.user, e.g. earthrisesrv) is
-- provisioned separately, see docs/installation.md.
--
-- Apply AFTER schema.sql / the migrations, against the game database:
--   sqlcmd -S localhost -E -d EarthRise -i Config\db\seed_dev_account.sql
--
-- Idempotent: re-running is a no-op once the account exists (guarded by username).
-- Azure-SQL-compatible: no cross-DB queries, no SQL Agent.
--
-- ─────────────────────────────────────────────────────────────────────────────
-- HOW THE HASH WAS PRODUCED  (§14: AccountStore::HashPassword)
--   PasswordHash = PBKDF2-HMAC-SHA512( password ‖ serverPepper, PasswordSalt,
--                                      Pbkdf2Iterations, 64 bytes )
-- The salt below is a fixed, well-known DEV salt (a salt is not secret — it only
-- has to match the stored hash, which it does here). The hash is bound to BOTH:
--   * auth.serverPepper      = "3556161927928253400"   (erserver.config.json)
--   * auth.pbkdf2Iterations  = 210000                  (erserver.config.json)
-- If you change EITHER value in the config, this hash stops verifying and login
-- fails — regenerate it (PowerShell, no project build needed) and replace the two
-- VARBINARY literals below:
--
--   $pw   = "player1-devpass" + "3556161927928253400"   # password ‖ pepper
--   $salt = [byte[]](-split "53 45 45 44 31 70 77 61 72 2d 70 6c 61 79 65 72 31 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20" | % {[Convert]::ToByte($_,16)})
--   $kdf  = [System.Security.Cryptography.Rfc2898DeriveBytes]::new(
--               [Text.Encoding]::UTF8.GetBytes($pw), $salt, 210000,
--               [Security.Cryptography.HashAlgorithmName]::SHA512)
--   '0x' + (($kdf.GetBytes(64) | % { $_.ToString('x2') }) -join '')
-- =============================================================================

SET NOCOUNT ON;
SET XACT_ABORT ON;

DECLARE @Username   NVARCHAR(64) = N'player1';
DECLARE @Iterations INT          = 210000;  -- must equal auth.pbkdf2Iterations
-- Fixed dev salt (32 bytes) + the PBKDF2-HMAC-SHA512 hash of "player1-devpass" ‖ pepper.
DECLARE @Salt VARBINARY(32) =
    0x5345454431707761722d706c6179657231202020202020202020202020202020;
DECLARE @Hash VARBINARY(64) =
    0x289a13f4f39520c351e8e3df638f22d7fc75e37eda2790fe24108b91a3a6ca9c1337ca0b679a2fdc1d548a7dc8bab49e8afc300dd73f0e1d754648b87c9d6ed2;

IF EXISTS (SELECT 1 FROM Accounts WHERE Username = @Username)
BEGIN
    PRINT 'Dev account "' + @Username + '" already exists - nothing to do.';
END
ELSE
BEGIN
    BEGIN TRANSACTION;

    INSERT INTO Accounts (Username, PasswordHash, PasswordSalt, Pbkdf2Iterations, Status)
        VALUES (@Username, @Hash, @Salt, @Iterations, 0 /* active */);

    DECLARE @AccountId BIGINT = CAST(SCOPE_IDENTITY() AS BIGINT);

    -- Register() creates a zero Wallet in the same transaction (EconomyStore's
    -- write-through assumes Wallets(AccountId) exists); mirror that here.
    INSERT INTO Wallets (AccountId, Balance) VALUES (@AccountId, 0);

    COMMIT TRANSACTION;

    PRINT 'Seeded dev account "' + @Username + '" (AccountId ' +
          CAST(@AccountId AS NVARCHAR(20)) + '). Log in with password: player1-devpass';
END
GO
