-- EarthRise — initial schema (M5 target)
-- Azure-SQL-compatible. No cross-DB queries, no FILESTREAM, no SQL Agent jobs.
-- All money/economy columns use BIGINT (integer credits) to avoid float rounding.
-- Versioned migrations live in db/migrations/.
--
-- Durability contract (§15):
--   Economy events  → write-through / transactional outbox  (zero-loss on crash)
--   Position/state  → write-behind, RPO ≤ a few seconds
--   Warm restart    → latest binary snapshot blob + event log since snapshot

-- ============================================================
-- Accounts & authentication (§14)
-- ============================================================
CREATE TABLE Accounts (
    AccountId       BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    Username        NVARCHAR(64)    NOT NULL,
    PasswordHash    VARBINARY(64)   NOT NULL,  -- PBKDF2-HMAC-SHA512 output
    PasswordSalt    VARBINARY(32)   NOT NULL,  -- per-account random salt
    -- Server pepper is applied in-process; never stored here.
    CreatedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME(),
    LastLoginAt     DATETIME2       NULL,
    Status          TINYINT         NOT NULL DEFAULT 0,  -- 0=active 1=banned 2=suspended
    LoginFailures   INT             NOT NULL DEFAULT 0,
    LockedUntil     DATETIME2       NULL,
    CONSTRAINT UQ_Accounts_Username UNIQUE (Username)
);

-- ============================================================
-- Sessions
-- ============================================================
CREATE TABLE Sessions (
    SessionId       BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    AccountId       BIGINT          NOT NULL REFERENCES Accounts(AccountId),
    Token           BINARY(32)      NOT NULL,   -- expiring session token
    ExpiresAt       DATETIME2       NOT NULL,
    CreatedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME(),
    RevokedAt       DATETIME2       NULL,
    CONSTRAINT UQ_Sessions_Token UNIQUE (Token)
);
CREATE INDEX IX_Sessions_AccountId ON Sessions (AccountId);

-- ============================================================
-- Player bases (mobile home base — §13)
-- ============================================================
CREATE TABLE Bases (
    BaseId          BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    AccountId       BIGINT          NOT NULL REFERENCES Accounts(AccountId),
    PosX            BIGINT          NOT NULL DEFAULT 0,   -- int64 metres
    PosY            BIGINT          NOT NULL DEFAULT 0,
    PosZ            BIGINT          NOT NULL DEFAULT 0,
    HP              INT             NOT NULL DEFAULT 1000,
    MaxHP           INT             NOT NULL DEFAULT 1000,
    StorageCredits  BIGINT          NOT NULL DEFAULT 0,
    StorageMass     BIGINT          NOT NULL DEFAULT 0,   -- in kg
    SafeZoneRadius  BIGINT          NOT NULL DEFAULT 5000, -- metres
    UpdatedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME(),
    CONSTRAINT UQ_Bases_AccountId UNIQUE (AccountId)
);

-- ============================================================
-- Ships (§13 — scout/harvester/fighter/builder)
-- ============================================================
CREATE TABLE Ships (
    ShipId          BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    BaseId          BIGINT          NOT NULL REFERENCES Bases(BaseId),
    ShipType        TINYINT         NOT NULL,   -- 0=scout 1=harvester 2=fighter 3=builder
    PosX            BIGINT          NOT NULL DEFAULT 0,
    PosY            BIGINT          NOT NULL DEFAULT 0,
    PosZ            BIGINT          NOT NULL DEFAULT 0,
    HP              INT             NOT NULL DEFAULT 100,
    MaxHP           INT             NOT NULL DEFAULT 100,
    UpdatedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME()
);
CREATE INDEX IX_Ships_BaseId ON Ships (BaseId);

-- ============================================================
-- Build queues
-- ============================================================
CREATE TABLE BuildQueue (
    BuildId         BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    BaseId          BIGINT          NOT NULL REFERENCES Bases(BaseId),
    ItemType        TINYINT         NOT NULL,
    QueuePosition   INT             NOT NULL DEFAULT 0,
    StartedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME(),
    CompletesAt     DATETIME2       NOT NULL
);
CREATE INDEX IX_BuildQueue_BaseId ON BuildQueue (BaseId);

-- ============================================================
-- Resource nodes (§13)
-- ============================================================
CREATE TABLE ResourceNodes (
    NodeId          BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    NodeType        TINYINT         NOT NULL,
    PosX            BIGINT          NOT NULL,
    PosY            BIGINT          NOT NULL,
    PosZ            BIGINT          NOT NULL,
    Remaining       BIGINT          NOT NULL,
    MaxCapacity     BIGINT          NOT NULL,
    UpdatedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME()
);

-- ============================================================
-- Zones (PvP / safe-zone definitions — §13)
-- ============================================================
CREATE TABLE Zones (
    ZoneId          BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    ZoneType        TINYINT         NOT NULL,   -- 0=safe 1=pvp
    CenterX         BIGINT          NOT NULL,
    CenterY         BIGINT          NOT NULL,
    CenterZ         BIGINT          NOT NULL,
    RadiusMetres    BIGINT          NOT NULL
);

-- ============================================================
-- Loot containers (§13 — loot-on-kill)
-- ============================================================
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
);

-- ============================================================
-- Economy outbox — transactional write-through (§15)
-- Rows inserted in the same transaction as the authoritative economy change.
-- A persistence thread drains this in order and deletes each row on success.
-- ============================================================
CREATE TABLE EconomyOutbox (
    OutboxId        BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    EventType       NVARCHAR(64)    NOT NULL,   -- 'trade','loot_claimed','build_complete',…
    Payload         NVARCHAR(MAX)   NOT NULL,   -- JSON blob of the event
    CreatedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME(),
    ProcessedAt     DATETIME2       NULL
);
CREATE INDEX IX_EconomyOutbox_Unprocessed ON EconomyOutbox (OutboxId) WHERE ProcessedAt IS NULL;

-- ============================================================
-- Warm-restart snapshots (§15)
-- A periodic binary blob of the full sim state; on restart, load the latest
-- snapshot and replay the EconomyOutbox events logged since it was taken.
-- ============================================================
CREATE TABLE SimSnapshots (
    SnapshotId      BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    SimTickNumber   BIGINT          NOT NULL,
    TakenAt         DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME(),
    Blob            VARBINARY(MAX)  NOT NULL
);
CREATE INDEX IX_SimSnapshots_Tick ON SimSnapshots (SimTickNumber DESC);
