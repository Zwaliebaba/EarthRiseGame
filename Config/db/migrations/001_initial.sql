-- Migration 001 — initial schema
-- Run once against a blank database.
-- Idempotent: wrapped in IF NOT EXISTS checks where Azure SQL supports it.

IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'Accounts')
BEGIN
    EXEC('
        CREATE TABLE Accounts (
            AccountId       BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
            Username        NVARCHAR(64)    NOT NULL,
            PasswordHash    VARBINARY(64)   NOT NULL,
            PasswordSalt    VARBINARY(32)   NOT NULL,
            CreatedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME(),
            LastLoginAt     DATETIME2       NULL,
            Status          TINYINT         NOT NULL DEFAULT 0,
            LoginFailures   INT             NOT NULL DEFAULT 0,
            LockedUntil     DATETIME2       NULL,
            CONSTRAINT UQ_Accounts_Username UNIQUE (Username)
        )
    ');
END
GO

IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'Sessions')
BEGIN
    EXEC('
        CREATE TABLE Sessions (
            SessionId       BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
            AccountId       BIGINT          NOT NULL REFERENCES Accounts(AccountId),
            Token           BINARY(32)      NOT NULL,
            ExpiresAt       DATETIME2       NOT NULL,
            CreatedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME(),
            RevokedAt       DATETIME2       NULL,
            CONSTRAINT UQ_Sessions_Token UNIQUE (Token)
        );
        CREATE INDEX IX_Sessions_AccountId ON Sessions (AccountId);
    ');
END
GO

IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'Bases')
BEGIN
    EXEC('
        CREATE TABLE Bases (
            BaseId          BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
            AccountId       BIGINT          NOT NULL REFERENCES Accounts(AccountId),
            PosX            BIGINT          NOT NULL DEFAULT 0,
            PosY            BIGINT          NOT NULL DEFAULT 0,
            PosZ            BIGINT          NOT NULL DEFAULT 0,
            HP              INT             NOT NULL DEFAULT 1000,
            MaxHP           INT             NOT NULL DEFAULT 1000,
            StorageCredits  BIGINT          NOT NULL DEFAULT 0,
            StorageMass     BIGINT          NOT NULL DEFAULT 0,
            SafeZoneRadius  BIGINT          NOT NULL DEFAULT 5000,
            UpdatedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME(),
            CONSTRAINT UQ_Bases_AccountId UNIQUE (AccountId)
        )
    ');
END
GO

IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'Ships')
BEGIN
    EXEC('
        CREATE TABLE Ships (
            ShipId          BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
            BaseId          BIGINT          NOT NULL REFERENCES Bases(BaseId),
            ShipType        TINYINT         NOT NULL,
            PosX            BIGINT          NOT NULL DEFAULT 0,
            PosY            BIGINT          NOT NULL DEFAULT 0,
            PosZ            BIGINT          NOT NULL DEFAULT 0,
            HP              INT             NOT NULL DEFAULT 100,
            MaxHP           INT             NOT NULL DEFAULT 100,
            UpdatedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME()
        );
        CREATE INDEX IX_Ships_BaseId ON Ships (BaseId);
    ');
END
GO

IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'BuildQueue')
BEGIN
    EXEC('
        CREATE TABLE BuildQueue (
            BuildId         BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
            BaseId          BIGINT          NOT NULL REFERENCES Bases(BaseId),
            ItemType        TINYINT         NOT NULL,
            QueuePosition   INT             NOT NULL DEFAULT 0,
            StartedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME(),
            CompletesAt     DATETIME2       NOT NULL
        );
        CREATE INDEX IX_BuildQueue_BaseId ON BuildQueue (BaseId);
    ');
END
GO

IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'ResourceNodes')
BEGIN
    EXEC('
        CREATE TABLE ResourceNodes (
            NodeId          BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
            NodeType        TINYINT         NOT NULL,
            PosX            BIGINT          NOT NULL,
            PosY            BIGINT          NOT NULL,
            PosZ            BIGINT          NOT NULL,
            Remaining       BIGINT          NOT NULL,
            MaxCapacity     BIGINT          NOT NULL,
            UpdatedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME()
        )
    ');
END
GO

IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'Zones')
BEGIN
    EXEC('
        CREATE TABLE Zones (
            ZoneId          BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
            ZoneType        TINYINT         NOT NULL,
            CenterX         BIGINT          NOT NULL,
            CenterY         BIGINT          NOT NULL,
            CenterZ         BIGINT          NOT NULL,
            RadiusMetres    BIGINT          NOT NULL
        )
    ');
END
GO

IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'LootContainers')
BEGIN
    EXEC('
        CREATE TABLE LootContainers (
            LootId          BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
            PosX            BIGINT          NOT NULL,
            PosY            BIGINT          NOT NULL,
            PosZ            BIGINT          NOT NULL,
            Credits         BIGINT          NOT NULL DEFAULT 0,
            Mass            BIGINT          NOT NULL DEFAULT 0,
            ExpiresAt       DATETIME2       NOT NULL,
            ClaimedByBaseId BIGINT          NULL REFERENCES Bases(BaseId),
            ClaimedAt       DATETIME2       NULL
        )
    ');
END
GO

IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'EconomyOutbox')
BEGIN
    EXEC('
        CREATE TABLE EconomyOutbox (
            OutboxId        BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
            EventType       NVARCHAR(64)    NOT NULL,
            Payload         NVARCHAR(MAX)   NOT NULL,
            CreatedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME(),
            ProcessedAt     DATETIME2       NULL
        );
        CREATE INDEX IX_EconomyOutbox_Unprocessed ON EconomyOutbox (OutboxId) WHERE ProcessedAt IS NULL;
    ');
END
GO

IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'SimSnapshots')
BEGIN
    EXEC('
        CREATE TABLE SimSnapshots (
            SnapshotId      BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
            SimTickNumber   BIGINT          NOT NULL,
            TakenAt         DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME(),
            Blob            VARBINARY(MAX)  NOT NULL
        );
        CREATE INDEX IX_SimSnapshots_Tick ON SimSnapshots (SimTickNumber DESC);
    ');
END
GO
