# EarthRise — Tech-Tree Outline

> Companion to `masterplan.md` §13.3 (progression). **Status:** DRAFT v0.1.
> **Model:** hybrid — research (vertical) unlocks **hull classes & module tiers**;
> **fitting + tactics** (horizontal) decide fights. **Catch-up flattening:** top
> tiers have diminishing returns so a fully-researched player still loses to a
> better-fit/better-piloted smaller fleet (R17).

---

## 1. How research works

- **Inputs:** **Datacores** (from anomalies/expeditions & NPC salvage — see
  `economy-crafting.md`) + a resource cost + **time**. Exploration → progression.
- **Output:** a **blueprint unlock** (a hull or module you can then *build* via the
  crafting economy). Research unlocks the *ability to build*; you still pay materials
  per item.
- **No power without play:** research gates *access*, not *auto-power*. You still must
  mine/craft/fit/fly. (Contrast with EVE passive skill points — that time-gated axis
  is **deferred**, masterplan §19.)
- **Catch-up:** early tiers are cheap/fast; each tier costs disproportionately more
  datacores for a **smaller** stat gain (diminishing returns). New players reach
  *relevance* (T1 + competitive T2 fits) quickly; full T3 mastery is the long tail.

---

## 2. The five research branches

```
                         ┌────────────────────────┐
                         │   CORE / FOUNDATIONS    │  (unlocked at start)
                         │  base systems, T1 hulls │
                         └───────────┬────────────┘
        ┌──────────────┬────────────┼────────────┬───────────────┐
        ▼              ▼             ▼             ▼               ▼
 ┌────────────┐ ┌────────────┐ ┌──────────┐ ┌───────────┐ ┌──────────────┐
 │  WEAPONRY  │ │  DEFENSE   │ │ MOBILITY │ │ INDUSTRY  │ │  ELECTRONICS │
 │ (offense)  │ │ (tank)     │ │ & HULLS  │ │ & ECONOMY │ │  (EWAR/sensor)│
 └────────────┘ └────────────┘ └──────────┘ └───────────┘ └──────────────┘
```

Each branch is **T1 → T2 → T3**. Cross-branch **prerequisites** force breadth (you
can't rush one branch to T3 without supporting tech).

---

## 3. Branch detail

### 3.1 Weaponry (offense)
| Tier | Unlocks | Prereq |
| --- | --- | --- |
| **T1** | Kinetic Railgun, Thermal Laser, EM Pulser (basic, swappable damage types) | Core |
| **T2** | Per-type specialists (long-range vs short-range), Damage Amp II, Tracking Enhancer | Weaponry T1 + Electronics T1 |
| **T3** | Heavy-hull main batteries, advanced ammo/charges, capital fire-support modules | Weaponry T2 + Mobility T2 (Heavy hull) |

### 3.2 Defense (tank)
| Tier | Unlocks | Prereq |
| --- | --- | --- |
| **T1** | Shield Booster, Armor Plate, basic Resist Hardeners (per type) | Core |
| **T2** | Active shield/armor reps, layered hardeners, buffer extenders | Defense T1 |
| **T3** | Heavy-hull buffer tanks, adaptive hardeners, base reinforcement | Defense T2 + Mobility T2 |

### 3.3 Mobility & Hulls
| Tier | Unlocks | Prereq |
| --- | --- | --- |
| **T1** | **Light & Medium hulls**, Afterburner | Core |
| **T2** | **Heavy hull**, Microwarp, propulsion mods | Mobility T1 + Industry T1 |
| **T3** | Hull specializations (role-tuned variants), base mobility/jump upgrades | Mobility T2 + Defense T2 |

### 3.4 Industry & Economy
| Tier | Unlocks | Prereq |
| --- | --- | --- |
| **T1** | **Harvester & Hauler**, Mining Laser, basic refining, build queue II | Core |
| **T2** | **Builder/field-construction**, efficient refining (better yields), cargo expanders | Industry T1 |
| **T3** | Advanced manufacturing (T3 component build), **territory-structure construction**, market/contract tools | Industry T2 + Electronics T2 |

### 3.5 Electronics (EWAR & sensors)
| Tier | Unlocks | Prereq |
| --- | --- | --- |
| **T1** | **Scout** scan, basic sensors, Warp-Disruptor (tackle) | Core |
| **T2** | **EWAR**: Jammer, Web, Sensor-Damp; expanded scan (anomaly tiers) | Electronics T1 |
| **T3** | Advanced EWAR, fleet sensor-sharing, deep-space probing (rare null sites) | Electronics T2 + Weaponry T2 |

---

## 4. Progression milestones (the player's months-long arc)

| Phase | Research focus | Capability unlocked | Where they play (§13.5) |
| --- | --- | --- | --- |
| **Onboarding** | Core + Mobility T1 + Industry T1 | mine, build T1 fleet, run easy anomalies | High-sec |
| **Specialize** | one combat branch T1→T2 + Electronics T1 | competitive small-fleet fits, tackle/EWAR | High → Low-sec |
| **Sustain** | Defense/Weaponry T2 + Industry T2 | logistics, refined economy, low-sec PvP & better sites | Low-sec |
| **Heavy** | Mobility T2 (Heavy hull) + cross-branch T2 | anchor fleets, harder invasions, deep sites | Low → Null |
| **Endgame** | T3 + Industry T3 | territory structures, capital fire-support, top sites | Null-sec (conquest) |

---

## 5. Catch-up math (illustrative)

| Tier | Datacore cost (rel.) | Stat gain over prev. | Time |
| --- | --- | --- | --- |
| T1 | 1× | baseline | hours |
| T2 | 5× | **+25%** | days |
| T3 | 25× | **+12%** | weeks |

> The shrinking gain per escalating cost is the deliberate flattening: a new player's
> T1/T2 fit is within ~12–35% of a veteran's T3 — closeable by **fitting + piloting**,
> not grind. This is the R17 mitigation made numeric.

---

## 6. Open questions (track)
- Are blueprints **account-permanent** once researched, or per-base? (Recommend
  account-permanent for retention.)
- Datacore faucet rate vs research cost (balance against anomaly spawn rates).
- Whether **territory ownership** should grant research bonuses (ties conquest →
  progression; risk of rich-get-richer — see masterplan R18).
- Reintroduce **time-gated training** as an optional parallel axis? (deferred, §19)

> See also: `combat-balance.md` (what the unlocks do in a fight) and
> `economy-crafting.md` (what it costs to build the unlocked hulls/modules).
