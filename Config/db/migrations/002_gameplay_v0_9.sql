-- Migration 002 — gameplay v0.9 (PvP/PvE depth)
-- Forward migration from 001. Brings the schema in line with masterplan §13 v0.9
-- and docs/design/{combat-balance,tech-tree,economy-crafting}.md.
-- Idempotent (guarded by sys.columns / sys.tables). Azure-SQL-compatible.
-- Pre-launch note: 001 created empty tables, so added required columns are safe.
--   FK columns that cannot be back-filled deterministically (e.g. HullItemDefId)
--   are added NULL here and seeded before go-live; schema.sql declares the final
--   NOT NULL shape for fresh installs.

-- ---- Helper note: dropping a defaulted column needs its default constraint gone
--      first; each drop below clears the auto-named default via dynamic SQL.
-- ---- Binding note: any DML (UPDATE/INSERT/SELECT) that reads a LEGACY column of an
--      already-existing table must run via EXEC(N'...'). SQL Server binds column names
--      of existing tables at batch-COMPILE time, so an `IF EXISTS(...legacy column...)`
--      guard does NOT stop a "Msg 207 Invalid column name" when the column is absent
--      (deferred name resolution covers missing tables, not missing columns). DDL like
--      ALTER TABLE DROP COLUMN is checked at runtime, so it stays guarded without EXEC.

-- ============================================================
-- Accounts: onboarding flag
-- ============================================================
IF NOT EXISTS (SELECT 1 FROM sys.columns WHERE object_id = OBJECT_ID('Accounts') AND name = 'TutorialDone')
    ALTER TABLE Accounts ADD TutorialDone BIT NOT NULL DEFAULT 0;
GO

-- ============================================================
-- Catalog: ItemDefs (canonical item-id space; seeded from game data)
-- ============================================================
IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'ItemDefs')
BEGIN
    EXEC('
        CREATE TABLE ItemDefs (
            ItemDefId   SMALLINT     NOT NULL IDENTITY(1,1) PRIMARY KEY,
            Code        NVARCHAR(48) NOT NULL,
            Category    TINYINT      NOT NULL,
            DisplayName NVARCHAR(96) NOT NULL,
            BaseMass    BIGINT       NOT NULL DEFAULT 0,
            CONSTRAINT UQ_ItemDefs_Code UNIQUE (Code)
        );
        CREATE INDEX IX_ItemDefs_Category ON ItemDefs (Category);
    ');
END
GO

-- ============================================================
-- Regions (tiered security) — replaces Zones
-- ============================================================
IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'Regions')
BEGIN
    EXEC('
        CREATE TABLE Regions (
            RegionId     INT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
            Name         NVARCHAR(64) NOT NULL,
            SecurityTier TINYINT      NOT NULL,
            CenterX      BIGINT       NOT NULL,
            CenterY      BIGINT       NOT NULL,
            CenterZ      BIGINT       NOT NULL,
            RadiusMetres BIGINT       NOT NULL,
            CONSTRAINT CK_Regions_Tier CHECK (SecurityTier IN (0,1,2))
        );
    ');
END
GO
IF EXISTS (SELECT 1 FROM sys.tables WHERE name = 'Zones')
    DROP TABLE Zones;   -- superseded by Regions + base-derived safe-zone bubbles
GO

-- ============================================================
-- Wallets + CurrencyLedger; migrate Bases.StorageCredits → Wallets
-- ============================================================
IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'Wallets')
BEGIN
    EXEC('
        CREATE TABLE Wallets (
            AccountId BIGINT   NOT NULL PRIMARY KEY REFERENCES Accounts(AccountId),
            Balance   BIGINT   NOT NULL DEFAULT 0,
            UpdatedAt DATETIME2 NOT NULL DEFAULT SYSUTCDATETIME(),
            CONSTRAINT CK_Wallets_NonNeg CHECK (Balance >= 0)
        );
    ');
END
GO
IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'CurrencyLedger')
BEGIN
    EXEC('
        CREATE TABLE CurrencyLedger (
            LedgerId     BIGINT       NOT NULL IDENTITY(1,1) PRIMARY KEY,
            AccountId    BIGINT       NOT NULL REFERENCES Accounts(AccountId),
            DeltaCredits BIGINT       NOT NULL,
            BalanceAfter BIGINT       NOT NULL,
            Reason       NVARCHAR(64) NOT NULL,
            RefType      NVARCHAR(32) NULL,
            RefId        BIGINT       NULL,
            CreatedAt    DATETIME2    NOT NULL DEFAULT SYSUTCDATETIME()
        );
        CREATE INDEX IX_CurrencyLedger_Account ON CurrencyLedger (AccountId, CreatedAt);
    ');
END
GO
-- Seed wallets from existing bases' credits (if the old column is still present).
IF EXISTS (SELECT 1 FROM sys.columns WHERE object_id = OBJECT_ID('Bases') AND name = 'StorageCredits')
BEGIN
    -- Dynamic SQL so the legacy column binds at EXEC time, where the IF guard actually
    -- protects it. A plain statement binds 'StorageCredits' at batch-compile time and
    -- fails ("Invalid column name") on a DB that never had the legacy column (e.g. one
    -- created from schema.sql, or a re-run after the column was dropped).
    EXEC(N'
        INSERT INTO Wallets (AccountId, Balance)
        SELECT b.AccountId, b.StorageCredits
        FROM Bases b
        WHERE NOT EXISTS (SELECT 1 FROM Wallets w WHERE w.AccountId = b.AccountId);');
END
GO

-- ============================================================
-- PlayerStandings
-- ============================================================
IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'PlayerStandings')
BEGIN
    EXEC('
        CREATE TABLE PlayerStandings (
            AccountId       BIGINT  NOT NULL REFERENCES Accounts(AccountId),
            TargetAccountId BIGINT  NOT NULL REFERENCES Accounts(AccountId),
            Standing        TINYINT NOT NULL DEFAULT 1,
            UpdatedAt       DATETIME2 NOT NULL DEFAULT SYSUTCDATETIME(),
            CONSTRAINT PK_PlayerStandings PRIMARY KEY (AccountId, TargetAccountId),
            CONSTRAINT CK_PlayerStandings_NoSelf CHECK (AccountId <> TargetAccountId)
        );
    ');
END
GO

-- ============================================================
-- Bases: layered HP, disable-not-destroy state, region, cargo, name
-- ============================================================
IF NOT EXISTS (SELECT 1 FROM sys.columns WHERE object_id = OBJECT_ID('Bases') AND name = 'Name')
    ALTER TABLE Bases ADD Name NVARCHAR(64) NOT NULL DEFAULT N'Mothership';
GO
IF NOT EXISTS (SELECT 1 FROM sys.columns WHERE object_id = OBJECT_ID('Bases') AND name = 'CurrentRegionId')
    ALTER TABLE Bases ADD CurrentRegionId INT NULL REFERENCES Regions(RegionId);
GO
IF NOT EXISTS (SELECT 1 FROM sys.columns WHERE object_id = OBJECT_ID('Bases') AND name = 'ShieldHp')
    ALTER TABLE Bases ADD
        ShieldHp INT NOT NULL DEFAULT 5000, MaxShieldHp INT NOT NULL DEFAULT 5000,
        ArmorHp  INT NOT NULL DEFAULT 5000, MaxArmorHp  INT NOT NULL DEFAULT 5000,
        HullHp   INT NOT NULL DEFAULT 5000, MaxHullHp   INT NOT NULL DEFAULT 5000,
        BaseState TINYINT NOT NULL DEFAULT 0, RetreatUntil DATETIME2 NULL,
        CargoUsedMass BIGINT NOT NULL DEFAULT 0, CargoCapMass BIGINT NOT NULL DEFAULT 1000000;
GO
-- Back-fill HullHp from the old HP if present, then drop legacy columns.
IF EXISTS (SELECT 1 FROM sys.columns WHERE object_id = OBJECT_ID('Bases') AND name = 'HP')
BEGIN
    -- Dynamic SQL: defer legacy-column (HP/MaxHP) binding to runtime (see note above).
    EXEC(N'UPDATE Bases SET HullHp = HP, MaxHullHp = MaxHP;');
    DECLARE @b_sql NVARCHAR(MAX) = N'';
    SELECT @b_sql = @b_sql + N'ALTER TABLE Bases DROP CONSTRAINT ' + QUOTENAME(dc.name) + N';'
    FROM sys.default_constraints dc
    JOIN sys.columns c ON c.object_id = dc.parent_object_id AND c.column_id = dc.parent_column_id
    WHERE dc.parent_object_id = OBJECT_ID('Bases')
      AND c.name IN ('HP','MaxHP','StorageCredits','StorageMass');
    IF LEN(@b_sql) > 0 EXEC(@b_sql);
    ALTER TABLE Bases DROP COLUMN HP, MaxHP, StorageCredits, StorageMass;
END
GO

-- ============================================================
-- Ships: hull/role, layered HP, fit linkage, state
-- ============================================================
IF NOT EXISTS (SELECT 1 FROM sys.columns WHERE object_id = OBJECT_ID('Ships') AND name = 'HullItemDefId')
    ALTER TABLE Ships ADD HullItemDefId SMALLINT NULL REFERENCES ItemDefs(ItemDefId); -- seeded pre go-live
GO
IF NOT EXISTS (SELECT 1 FROM sys.columns WHERE object_id = OBJECT_ID('Ships') AND name = 'HullClass')
    ALTER TABLE Ships ADD HullClass TINYINT NOT NULL DEFAULT 1; -- default medium
GO
IF NOT EXISTS (SELECT 1 FROM sys.columns WHERE object_id = OBJECT_ID('Ships') AND name = 'Role')
    ALTER TABLE Ships ADD Role TINYINT NOT NULL DEFAULT 1;      -- default fighter
GO
IF NOT EXISTS (SELECT 1 FROM sys.columns WHERE object_id = OBJECT_ID('Ships') AND name = 'Name')
    ALTER TABLE Ships ADD Name NVARCHAR(64) NULL;
GO
IF NOT EXISTS (SELECT 1 FROM sys.columns WHERE object_id = OBJECT_ID('Ships') AND name = 'ShieldHp')
    ALTER TABLE Ships ADD
        ShieldHp INT NOT NULL DEFAULT 100, MaxShieldHp INT NOT NULL DEFAULT 100,
        ArmorHp  INT NOT NULL DEFAULT 100, MaxArmorHp  INT NOT NULL DEFAULT 100,
        HullHp   INT NOT NULL DEFAULT 100, MaxHullHp   INT NOT NULL DEFAULT 100,
        ShipState TINYINT NOT NULL DEFAULT 0, InsuranceId BIGINT NULL;
GO
-- Map legacy ShipType (0=scout 1=harvester 2=fighter 3=builder) → Role; back-fill
-- HullHp from old HP; then drop legacy columns.
IF EXISTS (SELECT 1 FROM sys.columns WHERE object_id = OBJECT_ID('Ships') AND name = 'ShipType')
BEGIN
    -- Dynamic SQL: defer legacy-column (HP/MaxHP/ShipType) binding to runtime (see note
    -- above). ShipType→Role map: 0=scout→0, 1=harvester→5, 2=fighter→1, 3=builder→7.
    EXEC(N'UPDATE Ships SET
        HullHp = HP, MaxHullHp = MaxHP,
        Role = CASE ShipType WHEN 0 THEN 0
                             WHEN 1 THEN 5
                             WHEN 2 THEN 1
                             WHEN 3 THEN 7
                             ELSE 1 END;');
    DECLARE @s_sql NVARCHAR(MAX) = N'';
    SELECT @s_sql = @s_sql + N'ALTER TABLE Ships DROP CONSTRAINT ' + QUOTENAME(dc.name) + N';'
    FROM sys.default_constraints dc
    JOIN sys.columns c ON c.object_id = dc.parent_object_id AND c.column_id = dc.parent_column_id
    WHERE dc.parent_object_id = OBJECT_ID('Ships')
      AND c.name IN ('ShipType','HP','MaxHP');
    IF LEN(@s_sql) > 0 EXEC(@s_sql);
    ALTER TABLE Ships DROP COLUMN ShipType, HP, MaxHP;
END
GO

-- ============================================================
-- Fitting: ShipModules, FitTemplates, FitTemplateSlots
-- ============================================================
IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'ShipModules')
BEGIN
    EXEC('
        CREATE TABLE ShipModules (
            ShipModuleId    BIGINT   NOT NULL IDENTITY(1,1) PRIMARY KEY,
            ShipId          BIGINT   NOT NULL REFERENCES Ships(ShipId),
            ModuleItemDefId SMALLINT NOT NULL REFERENCES ItemDefs(ItemDefId),
            SlotType        TINYINT  NOT NULL,
            SlotIndex       TINYINT  NOT NULL,
            CONSTRAINT CK_ShipModules_Slot CHECK (SlotType IN (0,1,2)),
            CONSTRAINT UQ_ShipModules_Slot UNIQUE (ShipId, SlotType, SlotIndex)
        );
        CREATE INDEX IX_ShipModules_Ship ON ShipModules (ShipId);
    ');
END
GO
IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'FitTemplates')
BEGIN
    EXEC('
        CREATE TABLE FitTemplates (
            FitId         BIGINT   NOT NULL IDENTITY(1,1) PRIMARY KEY,
            AccountId     BIGINT   NOT NULL REFERENCES Accounts(AccountId),
            Name          NVARCHAR(64) NOT NULL,
            HullItemDefId SMALLINT NOT NULL REFERENCES ItemDefs(ItemDefId),
            CreatedAt     DATETIME2 NOT NULL DEFAULT SYSUTCDATETIME()
        );
        CREATE INDEX IX_FitTemplates_Account ON FitTemplates (AccountId);
        CREATE TABLE FitTemplateSlots (
            FitId           BIGINT   NOT NULL REFERENCES FitTemplates(FitId),
            SlotType        TINYINT  NOT NULL,
            SlotIndex       TINYINT  NOT NULL,
            ModuleItemDefId SMALLINT NOT NULL REFERENCES ItemDefs(ItemDefId),
            CONSTRAINT PK_FitTemplateSlots PRIMARY KEY (FitId, SlotType, SlotIndex)
        );
    ');
END
GO

-- ============================================================
-- Inventory: BaseInventory, ShipCargo
-- ============================================================
IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'BaseInventory')
BEGIN
    EXEC('
        CREATE TABLE BaseInventory (
            BaseId    BIGINT   NOT NULL REFERENCES Bases(BaseId),
            ItemDefId SMALLINT NOT NULL REFERENCES ItemDefs(ItemDefId),
            Quantity  BIGINT   NOT NULL DEFAULT 0,
            CONSTRAINT PK_BaseInventory PRIMARY KEY (BaseId, ItemDefId),
            CONSTRAINT CK_BaseInventory_NonNeg CHECK (Quantity >= 0)
        );
    ');
END
GO
IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'ShipCargo')
BEGIN
    EXEC('
        CREATE TABLE ShipCargo (
            ShipId    BIGINT   NOT NULL REFERENCES Ships(ShipId),
            ItemDefId SMALLINT NOT NULL REFERENCES ItemDefs(ItemDefId),
            Quantity  BIGINT   NOT NULL DEFAULT 0,
            CONSTRAINT PK_ShipCargo PRIMARY KEY (ShipId, ItemDefId),
            CONSTRAINT CK_ShipCargo_NonNeg CHECK (Quantity >= 0)
        );
    ');
END
GO

-- ============================================================
-- Tech tree: AccountResearch, AccountBlueprints
-- ============================================================
IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'AccountResearch')
BEGIN
    EXEC('
        CREATE TABLE AccountResearch (
            AccountId      BIGINT       NOT NULL REFERENCES Accounts(AccountId),
            ResearchCode   NVARCHAR(48) NOT NULL,
            Status         TINYINT      NOT NULL DEFAULT 0,
            DatacoresSpent BIGINT       NOT NULL DEFAULT 0,
            CompletedAt    DATETIME2    NULL,
            CONSTRAINT PK_AccountResearch PRIMARY KEY (AccountId, ResearchCode)
        );
    ');
END
GO
IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'AccountBlueprints')
BEGIN
    EXEC('
        CREATE TABLE AccountBlueprints (
            AccountId       BIGINT   NOT NULL REFERENCES Accounts(AccountId),
            OutputItemDefId SMALLINT NOT NULL REFERENCES ItemDefs(ItemDefId),
            Source          TINYINT  NOT NULL DEFAULT 0,
            AcquiredAt      DATETIME2 NOT NULL DEFAULT SYSUTCDATETIME(),
            CONSTRAINT PK_AccountBlueprints PRIMARY KEY (AccountId, OutputItemDefId)
        );
    ');
END
GO

-- ============================================================
-- BuildQueue: itemized output (replaces ItemType tinyint)
-- ============================================================
IF NOT EXISTS (SELECT 1 FROM sys.columns WHERE object_id = OBJECT_ID('BuildQueue') AND name = 'OutputItemDefId')
    ALTER TABLE BuildQueue ADD OutputItemDefId SMALLINT NULL REFERENCES ItemDefs(ItemDefId);
GO
IF NOT EXISTS (SELECT 1 FROM sys.columns WHERE object_id = OBJECT_ID('BuildQueue') AND name = 'Quantity')
    ALTER TABLE BuildQueue ADD Quantity INT NOT NULL DEFAULT 1;
GO
IF NOT EXISTS (SELECT 1 FROM sys.columns WHERE object_id = OBJECT_ID('BuildQueue') AND name = 'Delivered')
    ALTER TABLE BuildQueue ADD Delivered BIT NOT NULL DEFAULT 0;
GO
IF EXISTS (SELECT 1 FROM sys.columns WHERE object_id = OBJECT_ID('BuildQueue') AND name = 'ItemType')
BEGIN
    DECLARE @bq_sql NVARCHAR(MAX) = N'';
    SELECT @bq_sql = @bq_sql + N'ALTER TABLE BuildQueue DROP CONSTRAINT ' + QUOTENAME(dc.name) + N';'
    FROM sys.default_constraints dc
    JOIN sys.columns c ON c.object_id = dc.parent_object_id AND c.column_id = dc.parent_column_id
    WHERE dc.parent_object_id = OBJECT_ID('BuildQueue') AND c.name = 'ItemType';
    IF LEN(@bq_sql) > 0 EXEC(@bq_sql);
    ALTER TABLE BuildQueue DROP COLUMN ItemType;
END
GO

-- ============================================================
-- ResourceNodes: itemized raw output + region
-- ============================================================
IF NOT EXISTS (SELECT 1 FROM sys.columns WHERE object_id = OBJECT_ID('ResourceNodes') AND name = 'ResourceItemDefId')
    ALTER TABLE ResourceNodes ADD ResourceItemDefId SMALLINT NULL REFERENCES ItemDefs(ItemDefId);
GO
IF NOT EXISTS (SELECT 1 FROM sys.columns WHERE object_id = OBJECT_ID('ResourceNodes') AND name = 'RegionId')
    ALTER TABLE ResourceNodes ADD RegionId INT NULL REFERENCES Regions(RegionId);
GO
IF EXISTS (SELECT 1 FROM sys.columns WHERE object_id = OBJECT_ID('ResourceNodes') AND name = 'NodeType')
BEGIN
    DECLARE @rn_sql NVARCHAR(MAX) = N'';
    SELECT @rn_sql = @rn_sql + N'ALTER TABLE ResourceNodes DROP CONSTRAINT ' + QUOTENAME(dc.name) + N';'
    FROM sys.default_constraints dc
    JOIN sys.columns c ON c.object_id = dc.parent_object_id AND c.column_id = dc.parent_column_id
    WHERE dc.parent_object_id = OBJECT_ID('ResourceNodes') AND c.name = 'NodeType';
    IF LEN(@rn_sql) > 0 EXEC(@rn_sql);
    ALTER TABLE ResourceNodes DROP COLUMN NodeType;
END
GO

-- ============================================================
-- Markets: MarketOrders, MarketTrades
-- ============================================================
IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'MarketOrders')
BEGIN
    EXEC('
        CREATE TABLE MarketOrders (
            OrderId           BIGINT   NOT NULL IDENTITY(1,1) PRIMARY KEY,
            AccountId         BIGINT   NOT NULL REFERENCES Accounts(AccountId),
            RegionId          INT      NOT NULL REFERENCES Regions(RegionId),
            ItemDefId         SMALLINT NOT NULL REFERENCES ItemDefs(ItemDefId),
            Side              TINYINT  NOT NULL,
            Quantity          BIGINT   NOT NULL,
            QuantityRemaining BIGINT   NOT NULL,
            UnitPrice         BIGINT   NOT NULL,
            Status            TINYINT  NOT NULL DEFAULT 0,
            CreatedAt         DATETIME2 NOT NULL DEFAULT SYSUTCDATETIME(),
            ExpiresAt         DATETIME2 NOT NULL,
            CONSTRAINT CK_MarketOrders_Side CHECK (Side IN (0,1)),
            CONSTRAINT CK_MarketOrders_Qty CHECK (Quantity > 0 AND QuantityRemaining >= 0 AND UnitPrice > 0)
        );
        CREATE INDEX IX_MarketOrders_Book ON MarketOrders (RegionId, ItemDefId, Side, UnitPrice) WHERE Status = 0;
        CREATE INDEX IX_MarketOrders_Account ON MarketOrders (AccountId);
    ');
END
GO
IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'MarketTrades')
BEGIN
    EXEC('
        CREATE TABLE MarketTrades (
            TradeId         BIGINT   NOT NULL IDENTITY(1,1) PRIMARY KEY,
            BuyOrderId      BIGINT   NOT NULL REFERENCES MarketOrders(OrderId),
            SellOrderId     BIGINT   NOT NULL REFERENCES MarketOrders(OrderId),
            BuyerAccountId  BIGINT   NOT NULL REFERENCES Accounts(AccountId),
            SellerAccountId BIGINT   NOT NULL REFERENCES Accounts(AccountId),
            ItemDefId       SMALLINT NOT NULL REFERENCES ItemDefs(ItemDefId),
            RegionId        INT      NOT NULL REFERENCES Regions(RegionId),
            Quantity        BIGINT   NOT NULL,
            UnitPrice       BIGINT   NOT NULL,
            FeeCredits      BIGINT   NOT NULL DEFAULT 0,
            ExecutedAt      DATETIME2 NOT NULL DEFAULT SYSUTCDATETIME()
        );
        CREATE INDEX IX_MarketTrades_Item ON MarketTrades (ItemDefId, ExecutedAt);
    ');
END
GO

-- ============================================================
-- Territory: TerritoryStructures, TerritoryCaptureLog
-- ============================================================
IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'TerritoryStructures')
BEGIN
    EXEC('
        CREATE TABLE TerritoryStructures (
            StructureId      BIGINT   NOT NULL IDENTITY(1,1) PRIMARY KEY,
            RegionId         INT      NOT NULL REFERENCES Regions(RegionId),
            StructureType    TINYINT  NOT NULL,
            PosX             BIGINT   NOT NULL,
            PosY             BIGINT   NOT NULL,
            PosZ             BIGINT   NOT NULL,
            OwnerAccountId   BIGINT   NULL REFERENCES Accounts(AccountId),
            OwnerSince       DATETIME2 NULL,
            CaptureState     TINYINT  NOT NULL DEFAULT 0,
            ContestedBy      BIGINT   NULL REFERENCES Accounts(AccountId),
            CaptureProgress  INT      NOT NULL DEFAULT 0,
            ReinforcedUntil  DATETIME2 NULL,
            ShieldHp         INT      NOT NULL DEFAULT 10000,
            ArmorHp          INT      NOT NULL DEFAULT 10000,
            HullHp           INT      NOT NULL DEFAULT 10000,
            UpkeepPaidUntil  DATETIME2 NULL,
            YieldItemDefId   SMALLINT NULL REFERENCES ItemDefs(ItemDefId),
            YieldRatePerHour BIGINT   NOT NULL DEFAULT 0,
            UpdatedAt        DATETIME2 NOT NULL DEFAULT SYSUTCDATETIME(),
            CONSTRAINT CK_TerritoryStructures_State CHECK (CaptureState IN (0,1,2))
        );
        CREATE INDEX IX_TerritoryStructures_Region ON TerritoryStructures (RegionId);
        CREATE INDEX IX_TerritoryStructures_Owner ON TerritoryStructures (OwnerAccountId);
        CREATE TABLE TerritoryCaptureLog (
            CaptureLogId  BIGINT   NOT NULL IDENTITY(1,1) PRIMARY KEY,
            StructureId   BIGINT   NOT NULL REFERENCES TerritoryStructures(StructureId),
            FromAccountId BIGINT   NULL REFERENCES Accounts(AccountId),
            ToAccountId   BIGINT   NULL REFERENCES Accounts(AccountId),
            OccurredAt    DATETIME2 NOT NULL DEFAULT SYSUTCDATETIME()
        );
    ');
END
GO

-- ============================================================
-- Insurance + Ships.InsuranceId FK
-- ============================================================
IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'InsuranceContracts')
BEGIN
    EXEC('
        CREATE TABLE InsuranceContracts (
            ContractId    BIGINT   NOT NULL IDENTITY(1,1) PRIMARY KEY,
            AccountId     BIGINT   NOT NULL REFERENCES Accounts(AccountId),
            ShipId        BIGINT   NULL REFERENCES Ships(ShipId),
            HullItemDefId SMALLINT NOT NULL REFERENCES ItemDefs(ItemDefId),
            PremiumPaid   BIGINT   NOT NULL,
            PayoutCredits BIGINT   NOT NULL,
            Status        TINYINT  NOT NULL DEFAULT 0,
            PurchasedAt   DATETIME2 NOT NULL DEFAULT SYSUTCDATETIME(),
            ExpiresAt     DATETIME2 NOT NULL,
            PaidOutAt     DATETIME2 NULL,
            CONSTRAINT CK_Insurance_Status CHECK (Status IN (0,1,2,3))
        );
        CREATE INDEX IX_Insurance_Ship ON InsuranceContracts (ShipId);
        CREATE INDEX IX_Insurance_Account ON InsuranceContracts (AccountId);
    ');
END
GO
IF NOT EXISTS (SELECT 1 FROM sys.foreign_keys WHERE name = 'FK_Ships_Insurance')
    ALTER TABLE Ships ADD CONSTRAINT FK_Ships_Insurance
        FOREIGN KEY (InsuranceId) REFERENCES InsuranceContracts(ContractId);
GO

-- ============================================================
-- Loot: itemize containers (drop legacy Mass; add LootContainerItems)
-- ============================================================
IF EXISTS (SELECT 1 FROM sys.columns WHERE object_id = OBJECT_ID('LootContainers') AND name = 'Mass')
BEGIN
    DECLARE @lc_sql NVARCHAR(MAX) = N'';
    SELECT @lc_sql = @lc_sql + N'ALTER TABLE LootContainers DROP CONSTRAINT ' + QUOTENAME(dc.name) + N';'
    FROM sys.default_constraints dc
    JOIN sys.columns c ON c.object_id = dc.parent_object_id AND c.column_id = dc.parent_column_id
    WHERE dc.parent_object_id = OBJECT_ID('LootContainers') AND c.name = 'Mass';
    IF LEN(@lc_sql) > 0 EXEC(@lc_sql);
    ALTER TABLE LootContainers DROP COLUMN Mass;
END
GO
IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'LootContainerItems')
BEGIN
    EXEC('
        CREATE TABLE LootContainerItems (
            LootId    BIGINT   NOT NULL REFERENCES LootContainers(LootId),
            ItemDefId SMALLINT NOT NULL REFERENCES ItemDefs(ItemDefId),
            Quantity  BIGINT   NOT NULL,
            CONSTRAINT PK_LootContainerItems PRIMARY KEY (LootId, ItemDefId)
        );
    ');
END
GO

-- ============================================================
-- KillmailLog
-- ============================================================
IF NOT EXISTS (SELECT 1 FROM sys.tables WHERE name = 'KillmailLog')
BEGIN
    EXEC('
        CREATE TABLE KillmailLog (
            KillId              BIGINT   NOT NULL IDENTITY(1,1) PRIMARY KEY,
            VictimKind          TINYINT  NOT NULL,
            VictimAccountId     BIGINT   NULL REFERENCES Accounts(AccountId),
            VictimRefId         BIGINT   NULL,
            VictimHullItemDefId SMALLINT NULL REFERENCES ItemDefs(ItemDefId),
            KillerAccountId     BIGINT   NULL REFERENCES Accounts(AccountId),
            RegionId            INT      NULL REFERENCES Regions(RegionId),
            PosX                BIGINT   NOT NULL DEFAULT 0,
            PosY                BIGINT   NOT NULL DEFAULT 0,
            PosZ                BIGINT   NOT NULL DEFAULT 0,
            EstValueCredits     BIGINT   NOT NULL DEFAULT 0,
            OccurredAt          DATETIME2 NOT NULL DEFAULT SYSUTCDATETIME(),
            CONSTRAINT CK_Killmail_Kind CHECK (VictimKind IN (0,1))
        );
        CREATE INDEX IX_Killmail_Victim ON KillmailLog (VictimAccountId, OccurredAt);
        CREATE INDEX IX_Killmail_Killer ON KillmailLog (KillerAccountId, OccurredAt);
    ');
END
GO
