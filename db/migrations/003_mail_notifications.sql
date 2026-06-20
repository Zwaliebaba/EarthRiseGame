-- Migration 003 — communications: in-game mail + offline notifications
-- Forward migration from 002. Implements masterplan §24 (referenced from §15).
-- Idempotent (guarded by sys.tables). Azure-SQL-compatible.
-- Durable but NOT zero-loss-critical: these are inserted directly when generated,
-- not routed through EconomyOutbox.

-- ============================================================
-- Mail — persistent player-to-player messages (SenderAccountId NULL = system)
-- ============================================================
IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'Mail')
BEGIN
    EXEC('
        CREATE TABLE Mail (
            MailId             BIGINT        NOT NULL IDENTITY(1,1) PRIMARY KEY,
            SenderAccountId    BIGINT        NULL REFERENCES Accounts(AccountId),
            RecipientAccountId BIGINT        NOT NULL REFERENCES Accounts(AccountId),
            Subject            NVARCHAR(128) NOT NULL,
            Body               NVARCHAR(MAX) NOT NULL,
            SentAt             DATETIME2     NOT NULL DEFAULT SYSUTCDATETIME(),
            ReadAt             DATETIME2     NULL,
            RecipientDeleted   BIT           NOT NULL DEFAULT 0,
            SenderDeleted      BIT           NOT NULL DEFAULT 0
        );
        CREATE INDEX IX_Mail_Inbox  ON Mail (RecipientAccountId, SentAt) WHERE RecipientDeleted = 0;
        CREATE INDEX IX_Mail_Unread ON Mail (RecipientAccountId) WHERE ReadAt IS NULL AND RecipientDeleted = 0;
        CREATE INDEX IX_Mail_Sent   ON Mail (SenderAccountId, SentAt) WHERE SenderDeleted = 0;
    ');
END
GO

-- ============================================================
-- Notifications — server-generated event alerts
-- Type: 0=territory_attacked 1=territory_lost 2=market_filled 3=insurance_paid
--       4=killmail 5=build_complete 6=mail_received 7=invasion 8=system
-- ============================================================
IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'Notifications')
BEGIN
    EXEC('
        CREATE TABLE Notifications (
            NotificationId BIGINT        NOT NULL IDENTITY(1,1) PRIMARY KEY,
            AccountId      BIGINT        NOT NULL REFERENCES Accounts(AccountId),
            Type           TINYINT       NOT NULL,
            Payload        NVARCHAR(MAX) NULL,
            CreatedAt      DATETIME2     NOT NULL DEFAULT SYSUTCDATETIME(),
            ReadAt         DATETIME2     NULL,
            CONSTRAINT CK_Notifications_Type CHECK (Type BETWEEN 0 AND 8)
        );
        CREATE INDEX IX_Notifications_Account ON Notifications (AccountId, CreatedAt);
        CREATE INDEX IX_Notifications_Unread  ON Notifications (AccountId) WHERE ReadAt IS NULL;
    ');
END
GO
