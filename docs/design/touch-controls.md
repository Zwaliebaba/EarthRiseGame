# EarthRise — Touch Control Scheme

> Companion to `masterplan.md` §23 (input), §22.3 (overview/radar), R20. **Status:**
> DRAFT v0.1. **Prior art:** modelled on **EVE Echoes** (which shipped this genre — a
> persistent space MMO with fleet/ship command — successfully on touch). Target: UWP
> **tablets**; desktop mouse+keyboard is the same model with extra shortcuts (§23.2).

---

## 1. Design principles
1. **Overview-driven, not universe-dragging.** The **overview list + radar (§22.3)** is the
   primary selection/command surface; the 3D view is **camera-first**. This is the core
   reason touch works for EarthRise.
2. **The camera never lives on one finger.** One finger = select/command; **two fingers
   = camera**. This single rule removes the select-vs-pan ambiguity that breaks ported
   PC RTS controls (R20).
3. **Smart single action.** One tap resolves the *right* command from the target — no
   button-picking for the common case.
4. **Buttons over gestures for commands.** Persistent on-screen bars (smart-select,
   contextual commands, modules) replace right-click and keyboard hotkeys.
5. **Low APM by design.** Stances, formations, control groups, and **autopilot** mean a
   fight is "pick target → orbit/focus-fire", not per-ship micro.
6. **Large, scalable hit targets** (accessibility §22.5) — every interactive element
   respects the user HUD-scale setting.
7. **Parity, not a fork.** Touch and desktop emit the **same intents** (§8.4); only the
   input front-end differs.

---

## 2. Gesture table (in-space)

| Gesture | Action |
| --- | --- |
| **One-finger tap** on overview row / bracket / radar blip | **Select** that contact (replaces current selection) |
| One-finger tap on **selected** + tap another | Additive select (or use multi-select toggle) |
| **One-finger tap on the universe** | **Smart action** for current selection (§4) |
| **One-finger tap-and-hold** (on target or universe) | **Radial context menu** (recovers "right-click" — full command list) |
| One-finger tap on empty space (nothing selected) | Deselect / dismiss menus |
| **Two-finger drag** | **Pan camera** |
| **Pinch** | **Zoom camera** (ship-detail ⇄ tactical) |
| **Two-finger twist** | **Rotate/orbit camera** |
| Double-tap a contact | Center camera on it |
| Tap a **control-group button** | Recall that group; tap-hold = assign current selection |
| Tap **marquee toggle**, then one-finger drag | **Box-select** (secondary tool — off by default) |

> Box-select exists but is **opt-in** (behind the marquee toggle); grouping is normally
> done with smart-select buttons (§3), so one-finger drag stays free for nothing
> accidental.

---

## 3. On-screen controls (HUD)

| Element | Contents |
| --- | --- |
| **Smart-select bar** | *All combat · All of type · All harvesters · Idle · Group 1–9* |
| **Contextual command bar** | Move · Attack · Guard · **Orbit** · **Keep-at-range** · Approach · **Warp** · Jump · Retreat · Stop |
| **Module / ability buttons** | Per selected ship/base: weapons, tank, EWAR, reps, scan, jump-drive (with cooldown/fuel readout) |
| **Control-group bar** | 1–9 with member counts; tap = recall, tap-hold = assign |
| **Overview panel** | Sortable/filterable contact list (type/distance/velocity/IFF) — the main selection surface |
| **Radar disc** | Bearing + range rings + IFF + vertical offset (§22.3) |
| **Selected/target panels** | Shield/armor/hull + resists, range, modules; base fuel/jump cooldown |
| **Alerts** | Low-hull→retreat, under attack, jump ready, invasion — paired with audio cues (§22.5) |

All bars are dockable/edge-anchored and scale with the HUD-scale setting.

---

## 4. Smart single-action resolution
A one-finger tap (or the primary command) resolves by **what was tapped**, given the
current selection:

| Tapped target | Resulting intent |
| --- | --- |
| Empty space | **Move** to point (on the current camera plane / sector) |
| Hostile ship/structure (IFF) | **Lock + Attack** (or approach to weapon range) |
| Resource node | **Harvest** (harvester) / move (others) |
| Jump beacon | **Warp-to** then **jump** (if linked + fuel + off cooldown) |
| Anomaly / scannable | **Warp-to** / scan |
| Friendly ship (ally) | **Assist / guard / remote-rep** (logistics) |
| Own base / structure | **Dock / deposit / return** |
| Loot container | **Scoop** |

Ambiguous or non-default actions are always available via **tap-and-hold → radial menu**.

---

## 5. Out-of-space screens
Management screens (fitting, market, research, inventory, starmap, mail) are standard
touch UI — tap, scroll, drag-and-drop (e.g. drag a module into a fitting slot), pinch on
the **starmap** to zoom the beacon graph. These have no RTS ambiguity and reuse the
immediate-mode widget toolkit (§22).

---

## 6. Open questions / validation (R20)
- **Prototype first:** build the overview-driven scheme on a tablet and playtest a
  fleet engagement before committing UI polish budget.
- **Multi-select ergonomics** on touch (additive tap vs marquee) — which feels best?
- **Radial menu depth** — how many actions before it needs nesting/paging?
- **Left/right-handed** layouts; reachability on large tablets (thumb zones).
- **Haptics** for confirmations/alerts (where supported).
- Whether desktop should also default to the smart-action model (likely yes) with
  box-select purely as a power-user shortcut.

> See also: `masterplan.md` §23 (input), §22 (UI/HUD/radar), §13.12 (warp/jump).
