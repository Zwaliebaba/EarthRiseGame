-- EarthRise — database schema  (canonical "overall schema" document)
-- =============================================================================
-- Target: M5 (accounts/persistence) → M7 (sandbox: conquest, economy, PvE).
-- Reflects masterplan v0.9 gameplay (§13) and docs/design/{combat-balance,
-- tech-tree,economy-crafting}.md.
--
-- Azure-SQL-compatible: NO cross-DB queries, NO FILESTREAM, NO SQL Agent jobs.
-- All money/economy columns use BIGINT integer credits (no float rounding).
-- Quantities/mass are BIGINT. Positions are int64 metres (§6).
-- Versioned, forward-only migrations live in Config/db/migrations/.
--
-- DURABILITY CONTRACT (§15)
--   Economy events  → write-through / transactional outbox  (zero-loss on crash)
--                     [Wallets, CurrencyLedger, Inventory, MarketOrders/Trades,
--                      BuildQueue completion, TerritoryStructures ownership,
--                      InsuranceContracts, AccountBlueprints, loot claims]
--   Position/state  → write-behind, RPO ≤ a few seconds
--                     [*.Pos*, *.*Hp, Bases/Ships transient columns]
--   Warm restart    → latest SimSnapshots blob + EconomyOutbox replayed since it.
--
-- INTENTIONALLY NOT PERSISTED AS ROWS (live only in the SimSnapshots blob):
--   * NPC units (PvE AI), projectiles            — transient sim entities (§9)
--   * Anomaly sites, invasion events             — procedurally (re)spawned (§13.7)
--   * Live parties/fleets-grouping, interest sets — session/sim state (§13.8)
--   These are recreated from the snapshot on warm restart; normalizing them would
--   violate the write-behind boundary and bloat the hot path.
--
-- CATALOG / BALANCE BOUNDARY
--   Item *stats*, hull/module *stats*, crafting *recipes*, research *costs &
--   prerequisites*, anomaly/invasion *definitions* are GAME DATA (versioned with
--   the build, loaded by NeuronCore) — NOT stored here. SQL keeps only the
--   canonical item-id space (ItemDefs) so player-owned state has referential
--   integrity, plus the mutable player/economy/universe state itself.
-- =============================================================================


-- ============================================================
-- §14  Accounts & authentication
-- ============================================================
CREATE TABLE Accounts (
    AccountId       BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    Username        NVARCHAR(64)    NOT NULL,
    PasswordHash    VARBINARY(64)   NOT NULL,   -- PBKDF2-HMAC-SHA512 output
    PasswordSalt    VARBINARY(32)   NOT NULL,   -- per-account random salt
    -- Server pepper is applied in-process; never stored here.
    CreatedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME(),
    LastLoginAt     DATETIME2       NULL,
    Status          TINYINT         NOT NULL DEFAULT 0,  -- 0=active 1=banned 2=suspended
    LoginFailures   INT             NOT NULL DEFAULT 0,
    LockedUntil     DATETIME2       NULL,
    TutorialDone    BIT             NOT NULL DEFAULT 0,  -- onboarding chain (§13.9)
    CONSTRAINT UQ_Accounts_Username UNIQUE (Username)
);

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
-- §13.4  Currency: wallet + append-only ledger
-- Wallet is account-scoped (survives base retreat; markets/insurance are
-- account-level). Every balance change appends a CurrencyLedger row in the same
-- transaction (anti-dupe audit) and is mirrored to EconomyOutbox.
-- ============================================================
CREATE TABLE Wallets (
    AccountId       BIGINT          NOT NULL PRIMARY KEY REFERENCES Accounts(AccountId),
    Balance         BIGINT          NOT NULL DEFAULT 0,   -- integer credits
    UpdatedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME(),
    CONSTRAINT CK_Wallets_NonNeg CHECK (Balance >= 0)
);

CREATE TABLE CurrencyLedger (
    LedgerId        BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    AccountId       BIGINT          NOT NULL REFERENCES Accounts(AccountId),
    DeltaCredits    BIGINT          NOT NULL,             -- +credit / -debit
    BalanceAfter    BIGINT          NOT NULL,
    Reason          NVARCHAR(64)    NOT NULL,             -- 'market_buy','bounty','insurance_payout',…
    RefType         NVARCHAR(32)    NULL,                 -- 'MarketTrade','Kill','Build',…
    RefId           BIGINT          NULL,
    CreatedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME()
);
CREATE INDEX IX_CurrencyLedger_Account ON CurrencyLedger (AccountId, CreatedAt);
-- Defence in depth: a replayed economy event can't append a second ledger row (migration 004).
CREATE UNIQUE INDEX UX_CurrencyLedger_Ref ON CurrencyLedger (RefType, RefId) WHERE RefType IS NOT NULL AND RefId IS NOT NULL;


-- ============================================================
-- §13.8  Player standings (light diplomacy: ally/neutral/hostile)
-- Drives friendly-fire & engagement rules. No formal corps at launch.
-- ============================================================
CREATE TABLE PlayerStandings (
    AccountId       BIGINT          NOT NULL REFERENCES Accounts(AccountId),
    TargetAccountId BIGINT          NOT NULL REFERENCES Accounts(AccountId),
    Standing        TINYINT         NOT NULL DEFAULT 1,  -- 0=hostile 1=neutral 2=ally
    UpdatedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME(),
    CONSTRAINT PK_PlayerStandings PRIMARY KEY (AccountId, TargetAccountId),
    CONSTRAINT CK_PlayerStandings_NoSelf CHECK (AccountId <> TargetAccountId)
);


-- ============================================================
-- Catalog: canonical item-id space (seeded from game data — see boundary note).
-- Holds id + category + code + display only; balance stats live in game data.
-- Referenced by inventory, cargo, market, fits, blueprints, loot, build queue.
-- Category: 0=raw 1=refined 2=component 3=module 4=hull 5=ammo 6=blueprint
-- ============================================================
CREATE TABLE ItemDefs (
    ItemDefId       SMALLINT        NOT NULL IDENTITY(1,1) PRIMARY KEY,
    Code            NVARCHAR(48)    NOT NULL,   -- e.g. 'raw.ferrite','module.railgun.t1','hull.medium'
    Category        TINYINT         NOT NULL,
    DisplayName     NVARCHAR(96)    NOT NULL,
    BaseMass        BIGINT          NOT NULL DEFAULT 0,  -- kg per unit (cargo math)
    CONSTRAINT UQ_ItemDefs_Code UNIQUE (Code)
);
CREATE INDEX IX_ItemDefs_Category ON ItemDefs (Category);


-- ============================================================
-- §13.5  Universe structure — tiered-security regions (high → low → null)
-- A region is a spatial volume with a security tier. Sectors (§6.3) fall into a
-- region by position. Base safe-zone bubbles (§13.6) are derived at runtime from
-- Bases.SafeZoneRadius, not stored here.
-- SecurityTier: 0=high (no PvP) 1=low (PvP, no claims) 2=null (PvP + claimable)
-- ============================================================
CREATE TABLE Regions (
    RegionId        INT             NOT NULL IDENTITY(1,1) PRIMARY KEY,
    Name            NVARCHAR(64)    NOT NULL,
    SecurityTier    TINYINT         NOT NULL,
    CenterX         BIGINT          NOT NULL,
    CenterY         BIGINT          NOT NULL,
    CenterZ         BIGINT          NOT NULL,
    RadiusMetres    BIGINT          NOT NULL,
    CONSTRAINT CK_Regions_Tier CHECK (SecurityTier IN (0,1,2))
);


-- ============================================================
-- §13.1  Player bases — mobile mothership (capital, disable-not-destroy)
-- One base per account. Asset/identity = write-through; Pos/*Hp/State = write-behind.
-- BaseState: 0=active 1=retreating 2=disabled(cooldown). At low hull the base is
-- forced to retreat (RetreatUntil) + loses cargo — it is never destroyed (§13.1).
-- ============================================================
CREATE TABLE Bases (
    BaseId          BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    AccountId       BIGINT          NOT NULL REFERENCES Accounts(AccountId),
    Name            NVARCHAR(64)    NOT NULL DEFAULT N'Mothership',
    PosX            BIGINT          NOT NULL DEFAULT 0,    -- int64 metres
    PosY            BIGINT          NOT NULL DEFAULT 0,
    PosZ            BIGINT          NOT NULL DEFAULT 0,
    CurrentRegionId INT             NULL REFERENCES Regions(RegionId),
    ShieldHp        INT             NOT NULL DEFAULT 5000,
    MaxShieldHp     INT             NOT NULL DEFAULT 5000,
    ArmorHp         INT             NOT NULL DEFAULT 5000,
    MaxArmorHp      INT             NOT NULL DEFAULT 5000,
    HullHp          INT             NOT NULL DEFAULT 5000,
    MaxHullHp       INT             NOT NULL DEFAULT 5000,
    BaseState       TINYINT         NOT NULL DEFAULT 0,
    RetreatUntil    DATETIME2       NULL,                  -- emergency-jump cooldown
    SafeZoneRadius  BIGINT          NOT NULL DEFAULT 5000, -- defensive bubble (§13.5/13.6)
    CargoUsedMass   BIGINT          NOT NULL DEFAULT 0,    -- kg
    CargoCapMass    BIGINT          NOT NULL DEFAULT 1000000,
    UpdatedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME(),
    CONSTRAINT UQ_Bases_AccountId UNIQUE (AccountId),
    CONSTRAINT CK_Bases_State CHECK (BaseState IN (0,1,2))
);
CREATE INDEX IX_Bases_Region ON Bases (CurrentRegionId);


-- ============================================================
-- §13.1/§13.2  Ships — the player's fleet roster (6–12/player at launch)
-- HullClass: 0=light 1=medium 2=heavy 3=industrial
-- Role:      0=scout 1=fighter 2=heavy 3=logistics 4=ewar 5=harvester 6=hauler 7=builder
-- ShipState: 0=active 1=destroyed(pending cleanup)  (destroyed ships drop loot, may pay insurance)
-- Asset/ownership = write-through; Pos/*Hp = write-behind.
-- ============================================================
CREATE TABLE Ships (
    ShipId          BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    BaseId          BIGINT          NOT NULL REFERENCES Bases(BaseId),
    HullItemDefId   SMALLINT        NOT NULL REFERENCES ItemDefs(ItemDefId), -- category 4 (hull)
    HullClass       TINYINT         NOT NULL,
    Role            TINYINT         NOT NULL,
    Name            NVARCHAR(64)    NULL,
    PosX            BIGINT          NOT NULL DEFAULT 0,
    PosY            BIGINT          NOT NULL DEFAULT 0,
    PosZ            BIGINT          NOT NULL DEFAULT 0,
    ShieldHp        INT             NOT NULL DEFAULT 100,
    MaxShieldHp     INT             NOT NULL DEFAULT 100,
    ArmorHp         INT             NOT NULL DEFAULT 100,
    MaxArmorHp      INT             NOT NULL DEFAULT 100,
    HullHp          INT             NOT NULL DEFAULT 100,
    MaxHullHp       INT             NOT NULL DEFAULT 100,
    ShipState       TINYINT         NOT NULL DEFAULT 0,
    InsuranceId     BIGINT          NULL,    -- FK added after InsuranceContracts (below)
    UpdatedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME(),
    CONSTRAINT CK_Ships_HullClass CHECK (HullClass IN (0,1,2,3)),
    CONSTRAINT CK_Ships_Role CHECK (Role BETWEEN 0 AND 7)
);
CREATE INDEX IX_Ships_BaseId ON Ships (BaseId);


-- ============================================================
-- §13.2/§5  Fitting — modules actually fitted to a ship
-- SlotType: 0=high 1=mid 2=low.  One row per fitted module.
-- Fitting/refitting consumes/returns modules from BaseInventory (economy event).
-- ============================================================
CREATE TABLE ShipModules (
    ShipModuleId    BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    ShipId          BIGINT          NOT NULL REFERENCES Ships(ShipId),
    ModuleItemDefId SMALLINT        NOT NULL REFERENCES ItemDefs(ItemDefId), -- category 3 (module)
    SlotType        TINYINT         NOT NULL,
    SlotIndex       TINYINT         NOT NULL,
    CONSTRAINT CK_ShipModules_Slot CHECK (SlotType IN (0,1,2)),
    CONSTRAINT UQ_ShipModules_Slot UNIQUE (ShipId, SlotType, SlotIndex)
);
CREATE INDEX IX_ShipModules_Ship ON ShipModules (ShipId);

-- Saved loadout templates (quality-of-life: reapply a fit when rebuilding ships).
CREATE TABLE FitTemplates (
    FitId           BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    AccountId       BIGINT          NOT NULL REFERENCES Accounts(AccountId),
    Name            NVARCHAR(64)    NOT NULL,
    HullItemDefId   SMALLINT        NOT NULL REFERENCES ItemDefs(ItemDefId),
    CreatedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME()
);
CREATE INDEX IX_FitTemplates_Account ON FitTemplates (AccountId);

CREATE TABLE FitTemplateSlots (
    FitId           BIGINT          NOT NULL REFERENCES FitTemplates(FitId),
    SlotType        TINYINT         NOT NULL,
    SlotIndex       TINYINT         NOT NULL,
    ModuleItemDefId SMALLINT        NOT NULL REFERENCES ItemDefs(ItemDefId),
    CONSTRAINT PK_FitTemplateSlots PRIMARY KEY (FitId, SlotType, SlotIndex)
);


-- ============================================================
-- §13.4  Inventory — itemized stocks (raw → refined → components → products)
-- Base storage and ship cargo are the two container kinds. Quantities change as
-- economy events (mining, refining, crafting, trading, looting).
-- ============================================================
CREATE TABLE BaseInventory (
    BaseId          BIGINT          NOT NULL REFERENCES Bases(BaseId),
    ItemDefId       SMALLINT        NOT NULL REFERENCES ItemDefs(ItemDefId),
    Quantity        BIGINT          NOT NULL DEFAULT 0,
    CONSTRAINT PK_BaseInventory PRIMARY KEY (BaseId, ItemDefId),
    CONSTRAINT CK_BaseInventory_NonNeg CHECK (Quantity >= 0)
);

CREATE TABLE ShipCargo (
    ShipId          BIGINT          NOT NULL REFERENCES Ships(ShipId),
    ItemDefId       SMALLINT        NOT NULL REFERENCES ItemDefs(ItemDefId),
    Quantity        BIGINT          NOT NULL DEFAULT 0,
    CONSTRAINT PK_ShipCargo PRIMARY KEY (ShipId, ItemDefId),
    CONSTRAINT CK_ShipCargo_NonNeg CHECK (Quantity >= 0)
);


-- ============================================================
-- §13.3  Tech tree — research progress + unlocked blueprints (per account)
-- Research definitions (branches/tiers/costs/prereqs) are GAME DATA; we store the
-- account's progress against research *codes* and the blueprints it can build.
-- ResearchStatus: 0=in-progress 1=complete
-- ============================================================
CREATE TABLE AccountResearch (
    AccountId       BIGINT          NOT NULL REFERENCES Accounts(AccountId),
    ResearchCode    NVARCHAR(48)    NOT NULL,             -- e.g. 'weaponry.t2'
    Status          TINYINT         NOT NULL DEFAULT 0,
    DatacoresSpent  BIGINT          NOT NULL DEFAULT 0,
    CompletedAt     DATETIME2       NULL,
    CONSTRAINT PK_AccountResearch PRIMARY KEY (AccountId, ResearchCode)
);

-- Buildable recipes the account owns (from research, loot, or purchase).
-- Source: 0=research 1=loot 2=market 3=starter
CREATE TABLE AccountBlueprints (
    AccountId       BIGINT          NOT NULL REFERENCES Accounts(AccountId),
    OutputItemDefId SMALLINT        NOT NULL REFERENCES ItemDefs(ItemDefId), -- hull/module/ammo
    Source          TINYINT         NOT NULL DEFAULT 0,
    AcquiredAt      DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME(),
    CONSTRAINT PK_AccountBlueprints PRIMARY KEY (AccountId, OutputItemDefId)
);


-- ============================================================
-- §13.0  Build queue — manufacture ships/modules from inventory
-- Inputs are consumed from BaseInventory at enqueue (economy event); the finished
-- OutputItemDefId is delivered to inventory (or instantiated as a Ship) at CompletesAt.
-- ============================================================
CREATE TABLE BuildQueue (
    BuildId         BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    BaseId          BIGINT          NOT NULL REFERENCES Bases(BaseId),
    OutputItemDefId SMALLINT        NOT NULL REFERENCES ItemDefs(ItemDefId),
    Quantity        INT             NOT NULL DEFAULT 1,
    QueuePosition   INT             NOT NULL DEFAULT 0,
    StartedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME(),
    CompletesAt     DATETIME2       NOT NULL,
    Delivered       BIT             NOT NULL DEFAULT 0,
    CONSTRAINT CK_BuildQueue_Qty CHECK (Quantity > 0)
);
CREATE INDEX IX_BuildQueue_BaseId ON BuildQueue (BaseId);


-- ============================================================
-- §13.0/§13.4  Resource nodes — mineable sources of RAW items
-- Scarcity by region drives the risk→reward gradient (§13.5).
-- ============================================================
CREATE TABLE ResourceNodes (
    NodeId          BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    ResourceItemDefId SMALLINT      NOT NULL REFERENCES ItemDefs(ItemDefId), -- category 0 (raw)
    RegionId        INT             NULL REFERENCES Regions(RegionId),
    PosX            BIGINT          NOT NULL,
    PosY            BIGINT          NOT NULL,
    PosZ            BIGINT          NOT NULL,
    Remaining       BIGINT          NOT NULL,
    MaxCapacity     BIGINT          NOT NULL,
    UpdatedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME()
);
CREATE INDEX IX_ResourceNodes_Region ON ResourceNodes (RegionId);


-- ============================================================
-- §13.4  Markets — per-region buy/sell order books + trade history
-- Regional (not one global AH): price differs by region → hauling/piracy gameplay.
-- Side: 0=buy 1=sell.  OrderStatus: 0=open 1=filled 2=cancelled 3=expired
-- ============================================================
CREATE TABLE MarketOrders (
    OrderId         BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    AccountId       BIGINT          NOT NULL REFERENCES Accounts(AccountId),
    RegionId        INT             NOT NULL REFERENCES Regions(RegionId),
    ItemDefId       SMALLINT        NOT NULL REFERENCES ItemDefs(ItemDefId),
    Side            TINYINT         NOT NULL,
    Quantity        BIGINT          NOT NULL,
    QuantityRemaining BIGINT        NOT NULL,
    UnitPrice       BIGINT          NOT NULL,            -- credits per unit
    Status          TINYINT         NOT NULL DEFAULT 0,
    CreatedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME(),
    ExpiresAt       DATETIME2       NOT NULL,
    CONSTRAINT CK_MarketOrders_Side CHECK (Side IN (0,1)),
    CONSTRAINT CK_MarketOrders_Qty CHECK (Quantity > 0 AND QuantityRemaining >= 0 AND UnitPrice > 0)
);
-- Matching index: open orders for an item in a region, best price first.
CREATE INDEX IX_MarketOrders_Book ON MarketOrders (RegionId, ItemDefId, Side, UnitPrice)
    WHERE Status = 0;
CREATE INDEX IX_MarketOrders_Account ON MarketOrders (AccountId);

CREATE TABLE MarketTrades (
    TradeId         BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    BuyOrderId      BIGINT          NOT NULL REFERENCES MarketOrders(OrderId),
    SellOrderId     BIGINT          NOT NULL REFERENCES MarketOrders(OrderId),
    BuyerAccountId  BIGINT          NOT NULL REFERENCES Accounts(AccountId),
    SellerAccountId BIGINT          NOT NULL REFERENCES Accounts(AccountId),
    ItemDefId       SMALLINT        NOT NULL REFERENCES ItemDefs(ItemDefId),
    RegionId        INT             NOT NULL REFERENCES Regions(RegionId),
    Quantity        BIGINT          NOT NULL,
    UnitPrice       BIGINT          NOT NULL,
    FeeCredits      BIGINT          NOT NULL DEFAULT 0,  -- currency sink (§13.4)
    ExecutedAt      DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME()
);
CREATE INDEX IX_MarketTrades_Item ON MarketTrades (ItemDefId, ExecutedAt);


-- ============================================================
-- §13.6  Territorial conquest — claimable nullsec structures
-- CaptureState: 0=neutral 1=contested 2=owned
-- Capture = clear defenders + hold timer; defenders get a reinforcement window.
-- Ownership is individual at launch (§13.8); upkeep is a currency sink, yield pays
-- out while held (use-it-or-lose-it).
-- ============================================================
CREATE TABLE TerritoryStructures (
    StructureId     BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    RegionId        INT             NOT NULL REFERENCES Regions(RegionId),
    StructureType   TINYINT         NOT NULL,            -- 0=extractor 1=sensor 2=jump-beacon …
    PosX            BIGINT          NOT NULL,
    PosY            BIGINT          NOT NULL,
    PosZ            BIGINT          NOT NULL,
    OwnerAccountId  BIGINT          NULL REFERENCES Accounts(AccountId),
    OwnerSince      DATETIME2       NULL,
    CaptureState    TINYINT         NOT NULL DEFAULT 0,
    ContestedBy     BIGINT          NULL REFERENCES Accounts(AccountId),
    CaptureProgress INT             NOT NULL DEFAULT 0,  -- 0..100 %
    ReinforcedUntil DATETIME2       NULL,                -- defender window
    ShieldHp        INT             NOT NULL DEFAULT 10000,
    ArmorHp         INT             NOT NULL DEFAULT 10000,
    HullHp          INT             NOT NULL DEFAULT 10000,
    UpkeepPaidUntil DATETIME2       NULL,                -- lapse → decays/unclaims
    YieldItemDefId  SMALLINT        NULL REFERENCES ItemDefs(ItemDefId),
    YieldRatePerHour BIGINT         NOT NULL DEFAULT 0,
    UpdatedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME(),
    CONSTRAINT CK_TerritoryStructures_State CHECK (CaptureState IN (0,1,2))
);
CREATE INDEX IX_TerritoryStructures_Region ON TerritoryStructures (RegionId);
CREATE INDEX IX_TerritoryStructures_Owner ON TerritoryStructures (OwnerAccountId);

-- Capture/loss history (analytics + retention "your territory was taken" alerts).
CREATE TABLE TerritoryCaptureLog (
    CaptureLogId    BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    StructureId     BIGINT          NOT NULL REFERENCES TerritoryStructures(StructureId),
    FromAccountId   BIGINT          NULL REFERENCES Accounts(AccountId),
    ToAccountId     BIGINT          NULL REFERENCES Accounts(AccountId),
    OccurredAt      DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME()
);


-- ============================================================
-- §13.9  Ship insurance — loss mitigation (currency sink + risk buffer)
-- Premium paid up front (sink); on ship destruction, payout credits the wallet.
-- Status: 0=active 1=paid-out 2=expired 3=cancelled
-- ============================================================
CREATE TABLE InsuranceContracts (
    ContractId      BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    AccountId       BIGINT          NOT NULL REFERENCES Accounts(AccountId),
    ShipId          BIGINT          NULL REFERENCES Ships(ShipId),  -- the insured ship
    HullItemDefId   SMALLINT        NOT NULL REFERENCES ItemDefs(ItemDefId),
    PremiumPaid     BIGINT          NOT NULL,
    PayoutCredits   BIGINT          NOT NULL,
    Status          TINYINT         NOT NULL DEFAULT 0,
    PurchasedAt     DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME(),
    ExpiresAt       DATETIME2       NOT NULL,
    PaidOutAt       DATETIME2       NULL,
    CONSTRAINT CK_Insurance_Status CHECK (Status IN (0,1,2,3))
);
CREATE INDEX IX_Insurance_Ship ON InsuranceContracts (ShipId);
CREATE INDEX IX_Insurance_Account ON InsuranceContracts (AccountId);

-- Deferred FK from Ships.InsuranceId now that InsuranceContracts exists.
ALTER TABLE Ships ADD CONSTRAINT FK_Ships_Insurance
    FOREIGN KEY (InsuranceId) REFERENCES InsuranceContracts(ContractId);


-- ============================================================
-- §13.2  Loot containers — itemized loot-on-kill (ships only)
-- Bases are disable-not-destroy (§13.1) and drop a cargo fraction on retreat, not
-- a full container. Containers expire; first claimant wins (economy event).
-- ============================================================
CREATE TABLE LootContainers (
    LootId          BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    PosX            BIGINT          NOT NULL,
    PosY            BIGINT          NOT NULL,
    PosZ            BIGINT          NOT NULL,
    Credits         BIGINT          NOT NULL DEFAULT 0,
    ExpiresAt       DATETIME2       NOT NULL,
    ClaimedByBaseId BIGINT          NULL REFERENCES Bases(BaseId),
    ClaimedAt       DATETIME2       NULL
);

CREATE TABLE LootContainerItems (
    LootId          BIGINT          NOT NULL REFERENCES LootContainers(LootId),
    ItemDefId       SMALLINT        NOT NULL REFERENCES ItemDefs(ItemDefId),
    Quantity        BIGINT          NOT NULL,
    CONSTRAINT PK_LootContainerItems PRIMARY KEY (LootId, ItemDefId)
);


-- ============================================================
-- Killmail log — combat audit & social/retention feature (EVE-style killmails)
-- Append-only. Records destroyed ships and disabled bases.
-- VictimKind: 0=ship 1=base(disabled)
-- ============================================================
CREATE TABLE KillmailLog (
    KillId          BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    VictimKind      TINYINT         NOT NULL,
    VictimAccountId BIGINT          NULL REFERENCES Accounts(AccountId),
    VictimRefId     BIGINT          NULL,                -- ShipId or BaseId
    VictimHullItemDefId SMALLINT    NULL REFERENCES ItemDefs(ItemDefId),
    KillerAccountId BIGINT          NULL REFERENCES Accounts(AccountId), -- NULL = NPC/PvE
    RegionId        INT             NULL REFERENCES Regions(RegionId),
    PosX            BIGINT          NOT NULL DEFAULT 0,
    PosY            BIGINT          NOT NULL DEFAULT 0,
    PosZ            BIGINT          NOT NULL DEFAULT 0,
    EstValueCredits BIGINT          NOT NULL DEFAULT 0,
    OccurredAt      DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME(),
    CONSTRAINT CK_Killmail_Kind CHECK (VictimKind IN (0,1))
);
CREATE INDEX IX_Killmail_Victim ON KillmailLog (VictimAccountId, OccurredAt);
CREATE INDEX IX_Killmail_Killer ON KillmailLog (KillerAccountId, OccurredAt);


-- ============================================================
-- §24  Communications — in-game mail (persistent, player-to-player)
-- Durable but not zero-loss-critical (not routed through EconomyOutbox).
-- SenderAccountId NULL ⇒ system mail. Soft-deletes keep audit + a sent view.
-- (Attachments — credits/items — are a post-launch addition; see §24/§19.)
-- ============================================================
CREATE TABLE Mail (
    MailId             BIGINT       NOT NULL IDENTITY(1,1) PRIMARY KEY,
    SenderAccountId    BIGINT       NULL REFERENCES Accounts(AccountId),   -- NULL = system
    RecipientAccountId BIGINT       NOT NULL REFERENCES Accounts(AccountId),
    Subject            NVARCHAR(128) NOT NULL,
    Body               NVARCHAR(MAX) NOT NULL,
    SentAt             DATETIME2    NOT NULL DEFAULT SYSUTCDATETIME(),
    ReadAt             DATETIME2    NULL,
    RecipientDeleted   BIT          NOT NULL DEFAULT 0,    -- soft-delete from inbox
    SenderDeleted      BIT          NOT NULL DEFAULT 0     -- soft-delete from sent view
);
-- Inbox view (newest first); filtered unread index for unread counts.
CREATE INDEX IX_Mail_Inbox  ON Mail (RecipientAccountId, SentAt) WHERE RecipientDeleted = 0;
CREATE INDEX IX_Mail_Unread ON Mail (RecipientAccountId) WHERE ReadAt IS NULL AND RecipientDeleted = 0;
CREATE INDEX IX_Mail_Sent   ON Mail (SenderAccountId, SentAt) WHERE SenderDeleted = 0;

-- ============================================================
-- §24  Notifications — server-generated event alerts (surfaced on login + live)
-- Type: 0=territory_attacked 1=territory_lost 2=market_filled 3=insurance_paid
--       4=killmail 5=build_complete 6=mail_received 7=invasion 8=system
-- Payload carries refs (StructureId/OrderId/KillId/…) for deep-linking in the UI.
-- ============================================================
CREATE TABLE Notifications (
    NotificationId  BIGINT       NOT NULL IDENTITY(1,1) PRIMARY KEY,
    AccountId       BIGINT       NOT NULL REFERENCES Accounts(AccountId),
    Type            TINYINT      NOT NULL,
    Payload         NVARCHAR(MAX) NULL,           -- JSON: { refType, refId, … }
    CreatedAt       DATETIME2    NOT NULL DEFAULT SYSUTCDATETIME(),
    ReadAt          DATETIME2    NULL,
    CONSTRAINT CK_Notifications_Type CHECK (Type BETWEEN 0 AND 8)
);
CREATE INDEX IX_Notifications_Account ON Notifications (AccountId, CreatedAt);
CREATE INDEX IX_Notifications_Unread  ON Notifications (AccountId) WHERE ReadAt IS NULL;


-- ============================================================
-- §15  Economy outbox — transactional write-through
-- Rows inserted in the SAME transaction as the authoritative economy change.
-- A persistence thread drains this in order and marks each row processed.
-- ============================================================
CREATE TABLE EconomyOutbox (
    OutboxId        BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    EventType       NVARCHAR(64)    NOT NULL,   -- 'trade','loot_claimed','build_complete','insurance_payout','territory_captured',…
    Payload         NVARCHAR(MAX)   NOT NULL,   -- JSON blob of the event
    IdempotencyKey  BIGINT          NULL,       -- exactly-once dedupe for ordered drain/replay (migration 004)
    CreatedAt       DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME(),
    ProcessedAt     DATETIME2       NULL
);
CREATE INDEX IX_EconomyOutbox_Unprocessed ON EconomyOutbox (OutboxId) WHERE ProcessedAt IS NULL;
-- A replayed drain cannot insert a duplicate effect (zero-loss + no double-credit, §15).
CREATE UNIQUE INDEX UX_EconomyOutbox_IdemKey ON EconomyOutbox (IdempotencyKey) WHERE IdempotencyKey IS NOT NULL;


-- ============================================================
-- §15  Warm-restart snapshots
-- Periodic binary blob of full sim state (incl. transient NPC/anomaly/invasion
-- state). On restart, load the latest snapshot then replay EconomyOutbox since it.
-- ============================================================
CREATE TABLE SimSnapshots (
    SnapshotId      BIGINT          NOT NULL IDENTITY(1,1) PRIMARY KEY,
    SimTickNumber   BIGINT          NOT NULL,
    OutboxWatermark BIGINT          NOT NULL DEFAULT 0,  -- max OutboxId reflected in Blob; restart replays OutboxId > this (migration 004)
    TakenAt         DATETIME2       NOT NULL DEFAULT SYSUTCDATETIME(),
    Blob            VARBINARY(MAX)  NOT NULL
);
CREATE INDEX IX_SimSnapshots_Tick ON SimSnapshots (SimTickNumber DESC);
