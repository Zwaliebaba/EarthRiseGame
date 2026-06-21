# EarthRise — HUD Layout & Screen Flow

> Companion to `masterplan.md` §22 (UI/HUD/radar), §23 (input), §13.12 (navigation).
> **Status:** DRAFT v0.1 — wireframes (ASCII) + navigation map for review. Built on the
> CanvasRenderer immediate-mode toolkit (§22); **DPI-aware, anchor-based, HUD-scalable**
> (§22.5); same layout on desktop & touch (touch just gets larger hit targets and the
> two-finger camera, §23.3). All text via the string table (§22.4).

---

## 1. Anchor & layout model
- **Anchor zones**, not absolute pixels: `TL TC TR / ML . MR / BL BC BR`, each panel
  pinned to a corner/edge and offset by a **HUD-scale** factor (user setting, §22.5).
- **Safe-area aware** (tablet rounded corners / notches); panels are **dockable** and
  **collapsible**; the center is kept clear for the 3D scene + bracket overlay (§22.3).
- **Z-order:** 3D scene -> bracket overlay -> HUD panels -> modal screens -> alerts/toasts.

---

## 2. Main HUD (in-space) — wireframe

```
+----------------------------------------------------------------------------+
| [= Menu]  Credits 1.2M   Fuel ####-   UTC 14:32      REGION: Veil (LOW)    |  TOP BAR
+---------------+------------------------------------------+-----------------+
| OVERVIEW      |                                          |  SELECTED       |
| (filters v)   |                                          |  +-----------+  |
| +-----------+ |                                          |  | Mothership|  |
| |#Hostile  3| |                                          |  | S #####   |  |
| | Frigate 12| |              3D SCENE                     |  | A ####-   |  |
| |~Neutral  8| |        (bracket overlay: IFF             |  | H ####    |  |
| |o Node    5| |         brackets + off-screen arrows)    |  | res K/T/E |  |
| |*Beacon  22| |                                          |  +-----------+  |
| | ...sort.. | |                  .  .                     |  | TARGET    |  |
| +-----------+ |                 .    .                    |  | Frigate   |  |
| [AllCbt][Typ] |                  .  .                     |  | S- A# H#  |  |
| SMART-SELECT  |                                          |  | 12 km     |  |
|               |                                          |  +-----------+  |
+---------------+--------------------------+---------------+-----------------+
| GROUPS [1.4][2.3][3][4]                  |  RADAR        MODULES           |
| CMD: Move Atk Guard Orbit Keep           |    .-''-.     [Gun][Gun]        |
|      Warp Jump Retreat Stop              |   ( o  + )    [Shld][EWAR][Rep] |
| (contextual, per selection)              |    '-..-'     (cooldown/fuel)   |
+------------------------------------------+--------------------------------+
| CHAT > [Local] Pilot_42: gf      |   (!) ALERT: "Hull critical!"          |  BOTTOM
+----------------------------------------------------------------------------+
```

Legend: `#`=hostile `~`=neutral `o`=resource node `*`=jump beacon; `S/A/H` =
shield/armor/hull bars (`#`=full, `-`=partial); `(!)`=alert toast.

### Panel map
| Zone | Panel | Notes (refs) |
| --- | --- | --- |
| **TC/TR** | Top bar | credits, **jump fuel**, universe-clock UTC (§26), current **region + security tier** (§13.5) |
| **TL/ML** | **Overview list** | primary selection/command surface; filters + **smart-select** bar (§23.1) |
| **Center** | 3D scene + **bracket overlay** | IFF brackets, off-screen arrows; camera-first (§22.3) |
| **TR/MR** | Selected + Target panels | shield/armor/hull + resists, range, modules; base fuel/jump cooldown (§13.2) |
| **BL** | Control-group bar | 1-9 with counts; recall/assign (§23) |
| **BC** | Contextual command bar + Module bar | smart/context commands; per-selection module buttons w/ cooldown+fuel |
| **BR** | **Radar disc** | bearing + range rings + IFF + vertical offset (§22.3) |
| **BL/BC** | Chat + Alerts/toasts | channels (§24); alerts paired with **audio cues** (§22.5) |

> **Combat-minimal mode** (toggle): collapse to overview + radar + command/module bars +
> selected/target only, hiding chrome — keeps the Darwinia "dark void" center clean.

---

## 3. Screen-flow map

```
            +------------+
            | BOOT/LOGIN |  account + version gate (§14, §8.5)
            +-----+------+
                  v
            +------------+   first run -> +----------------------+
            |  MAIN HUD  |--------------->| ONBOARDING OVERLAY   | guided chain (§13.9)
            | (in-space) |<---------------+----------------------+
            +-+-+-+-+-+--+
   +----------+ | | | +-----------+
   v            v | v             v
+--------+ +-------+ +---------+ +----------+
|STARMAP | |FITTING| | MARKET  | | RESEARCH |   overlays over a dimmed HUD
|(nav,   | |(slots,| |(orders, | |(tech tree|   (game keeps running -- not paused)
| jump)  | | PG/CPU| |  fees)  | |  queue)  |
+--------+ +-------+ +---------+ +----------+
   |            |         |            |
   +----- all reachable from the [= Menu] / hotbar; ESC/back closes -----+
                  v               v              v
            +----------+   +----------+   +----------+
            |INVENTORY |   |  SOCIAL  |   |  MAIL /  |   (§24)
            |/ CARGO / |   | standings|   |  NOTIFS  |
            | BUILD Q  |   | party/chat|  +----------+
            +----------+   +----------+
                                            +----------+
            [= Menu] also -> --------------->| SETTINGS | gfx/audio/keys/a11y/lang (§25)
                                            +----------+
```

- **Modeless where possible:** management screens are **overlays over a dimmed,
  still-running HUD** (the universe is live — §26), not full context switches; the radar/
  alerts stay visible so you're never blind while fitting or trading.
- **Universal back:** ESC / on-screen back / two-finger swipe-down closes the top overlay.
- **Deep-links:** notifications (§24) and killmails open the relevant screen (e.g.
  "market order filled" -> Market; "territory attacked" -> Starmap at that structure).

---

## 4. Per-screen layout notes
- **Starmap / navigation:** node-graph of the **jump-beacon network** (§13.12); nodes
  colored by **security tier**, owned territory highlighted; tap a beacon -> **Set
  destination** -> autopilot route preview (jumps, fuel, ETA). Pinch-zoom (touch) / wheel
  (desktop).
- **Fitting:** hull silhouette with **High/Mid/Low slot rows** + **PG/CPU budget bars**;
  drag modules from a side list into slots; **save/load fit templates** (§13.2); shows
  resulting EHP / damage-type profile (links `combat-balance.md`).
- **Market:** per-region **order book** (buy/sell ladders) + price history; place order
  with **fee preview** (sink, §13.4); "my orders" tab.
- **Research:** the 5-branch tech tree (`tech-tree.md`) with tiers, **datacore costs**,
  prereqs, and the active research queue.
- **Inventory/cargo & build:** base storage + ship cargo tabs; **build queue** with
  input-cost check and ETA.
- **Social:** standings (set ally/neutral/hostile, drives IFF §22.3), party/fleet roster,
  chat channel manager.
- **Mail/notifications:** inbox/sent/compose; notification feed with unread badges (§24).
- **Settings:** graphics (§11.1 options), **audio bus volumes** (§11.3), **rebindable
  keymap**, **HUD scale + accessibility**, language (§22.4/§25).

---

## 5. Alerts & feedback
- **Toasts** (BC) for transient events; **persistent alert strip** for sustained danger
  (under attack / hull critical / fuel low / jump ready), each with a **distinct audio
  cue** (§22.5) and a tap-to-focus deep-link.
- **Selection echo:** selecting in overview highlights the bracket + radar blip and vice
  versa (one shared selection, §23.4).

---

## 6. Open questions (validation)
- Default panel docking & which panels are in **combat-minimal** mode.
- Overview column set & default sort (distance vs threat).
- Tablet thumb-zone reachability for the command/module bars (left/right-handed).
- Whether the radar disc and 2D minimap/starmap should share a widget.
- Controller layout (deferred — §19), if ever added.

> See also: `masterplan.md` §22 / §23, `touch-controls.md`, `combat-balance.md`,
> `tech-tree.md`, `economy-crafting.md`.
