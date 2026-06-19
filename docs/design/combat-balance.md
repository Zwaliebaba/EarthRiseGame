# EarthRise — Combat Balance & Ship-Role Spreadsheet

> Companion to `masterplan.md` §13.2 (combat), §13.1 (fleet command). **Status:**
> DRAFT v0.1 — first-pass balance numbers for review and bot tuning. All numbers are
> **relative baselines** meant to be tuned against headless bot fights (ERHeadless),
> not final values. Stats are stored data-driven (no hard-coded balance in code).

---

## 1. Design goals (the balance contract)

1. **Composition > numbers.** A balanced 8-ship fleet should beat a same-cost
   single-role blob. (Test: mono-DPS fleet vs mixed fleet of equal build cost.)
2. **Fit > tier.** The right damage type / resist profile should let a lower-tier,
   well-fit fleet beat a higher-tier wrong-fit fleet of similar cost (R17 catch-up).
3. **Everything is a counter to something.** No dominant ship/module/damage type.
4. **Readable at RTS scale.** With a 6–12 ship fleet, a player can tell at a glance
   what each ship is doing. Roles are visually distinct (Darwinia silhouettes).
5. **Disable-not-destroy for the base** (masterplan §13.1): the mothership is a
   capital that retreats at low HP; it is never permanently destroyed.

---

## 2. Defense & damage model

### 2.1 Three defense layers (depleted outside-in)
| Layer | Regenerates? | Repaired by | Notes |
| --- | --- | --- | --- |
| **Shield** | Yes (passive + active) | Shield-logistics modules | First line; best vs alpha if buffered. |
| **Armor** | No | Armor-logistics (remote rep) | Buffer; slow to rep, no passive regen. |
| **Hull** | No | Builder/repair ships only (slow) | Danger zone; near-hull = retreat/insurance. |

Each layer has its **own resist profile** per damage type, so fits choose what they
tank against.

### 2.2 Damage types & the counter triangle
Launch types: **Kinetic (K) / Thermal (T) / EM (E)**. *(Explosive added post-launch.)*

```
        Kinetic ─ strong vs ─▶ Armor-heavy fits
           ▲                        │
   strong vs                    strong vs
   Shield-heavy                 Hull / structure
           │                        ▼
          EM ◀─ strong vs ─ Thermal
```

**Resist intent (typical, %):** shields naturally resist EM *poorly*, kinetic
*well*; armor resists EM/kinetic *well*, thermal *poorly*; hull is flat. So:

| Attacker \ defender | Shield-tanked | Armor-tanked | Hull-exposed |
| --- | --- | --- | --- |
| **EM** | ✅ strong | ⚠️ ok | ▫️ flat |
| **Thermal** | ⚠️ ok | ✅ strong | ▫️ flat |
| **Kinetic** | ❌ weak | ⚠️ ok | ▫️ flat |

> Implication: scout the enemy tank → bring the matching damage type. This is the
> core fitting decision (masterplan §13.2).

### 2.3 Damage formula (server-authoritative, shared sim rule)
```
effective_dmg = base_dmg × (1 - resist[layer][type]) × tracking_factor × falloff_factor
```
- `tracking_factor` ∈ [0,1]: small/fast targets reduce hits from big slow turrets.
- `falloff_factor` ∈ [0,1]: 1.0 within optimal range, decays through falloff to 0.

---

## 3. Hull classes (size ladder)

| Class | Tier gate | Cost (rel.) | Speed | Sig (hit size) | Slots H/M/L | Role fit |
| --- | --- | --- | --- | --- | --- | --- |
| **Light** (frigate) | T1 | 1.0 | ★★★★★ | tiny | 2/2/1 | scout, tackle, light DPS |
| **Medium** (cruiser) | T1 | 3.5 | ★★★☆ | small | 3/3/3 | DPS, logistics, EWAR, harvester |
| **Heavy** (battlecruiser) | T2 | 9 | ★★☆ | large | 4/3/4 | anchor DPS, tank |
| **Industrial** | T1 | 4 | ★★☆ | large | 1/2/2 | hauler, builder/logistics |
| **Mothership-base** | (start) | — | ★ | huge | special | capital; **disable-not-destroy** |

- **Fleet cap:** 6–12 ships (default 8) + the base (masterplan §13.1). Heavy hulls
  cost more "fleet weight," nudging mixed comps over all-heavy.
- **Signature (sig):** small ships are hard for big guns to track → light tackle
  survives under battleship fire; this is the size rock-paper-scissors.

---

## 4. Ship roles (the spreadsheet)

Baselines are **multipliers on the class** (1.0 = class default). Tune in ERHeadless.

| Role | Archetype | DPS | EHP | Speed | Range | Special | Primary class |
| --- | --- | --- | --- | --- | --- | --- | --- |
| **Scout** | tackle | 0.5 | 0.6 | 1.6 | sensor 2.0 | scan, **warp-disrupt (tackle)**, fog reveal | Light |
| **Fighter** | DPS | 1.4 | 1.0 | 1.0 | 1.0 | focus-fire DPS, damage-type swappable | Medium |
| **Heavy** | anchor/tank | 1.2 | 2.2 | 0.6 | 1.2 | buffer tank, holds the line | Heavy |
| **Logistics** | logi | 0.2 | 1.1 | 0.9 | rep 1.5 | **remote shield/armor rep** | Medium |
| **EWAR** | ewar | 0.3 | 0.8 | 1.1 | 1.4 | jam / web (slow) / sensor-damp / disrupt | Medium |
| **Harvester** | industry | 0.1 | 0.9 | 0.8 | — | mine nodes → cargo (eXploit) | Medium |
| **Hauler** | industry | 0.0 | 1.2 | 0.7 | — | large cargo (trade/logistics) | Industrial |
| **Builder** | industry/logi | 0.1 | 1.0 | 0.7 | rep 1.0 | field construction + hull repair | Industrial |

EHP = effective HP across all layers after resists.

### 4.1 Why each role exists (counter web)
- **Scout** counters runners (tackle holds them) → enables kills; countered by EWAR
  (jammed scout can't hold tackle) and by fast ships breaking range.
- **Fighter** is raw DPS; countered by **Logistics** (out-reps incoming) and by
  **EWAR** (jammed fighters miss).
- **Heavy** anchors and tanks alpha; countered by **swarms of Light** (tracking) and
  by sustained DPS once logi is removed.
- **Logistics** makes a fleet durable; **the** priority kill → drives "primary the
  logi" target-calling. Countered by EWAR (jam the logi) and tackle (pin it).
- **EWAR** is the force multiplier; fragile, must be protected; countered by tackle +
  fast DPS reaching it.

> **Emergent loop:** tackle the logi → jam the logi → kill the logi → then the fleet
> melts. That target-priority dance is the skill expression at RTS scale.

---

## 5. Module fitting (slots & budget)

Each hull has **High / Mid / Low** slots + a **Power(PG)/CPU** budget. Fitting is the
join between the tech tree (`tech-tree.md`) and combat.

| Slot | Typical modules | Examples |
| --- | --- | --- |
| **High** | weapons, remote rep, mining | Kinetic Railgun, Thermal Laser, EM Pulser, Remote Shield Rep, Mining Laser |
| **Mid** | shield, EWAR, propulsion, tackle | Shield Booster, Jammer, Web, Warp-Disruptor, Afterburner |
| **Low** | armor, hull, damage/tracking mods | Armor Plate, Resist Hardener, Damage Amp, Tracking Enhancer |

- **Tiers T1→T2→T3** (tech tree): higher tiers cost more PG/CPU and rarer materials;
  **diminishing returns** so T1 stays viable (R17 / masterplan §13.3).
- **Fitting constraint = the trade-off:** you can't fit max tank + max gank + max
  EWAR; PG/CPU forces specialization → roles.

### 5.1 Example fit — Medium "Fighter" (anti-shield)
| Slot | Module | Cost |
| --- | --- | --- |
| High ×3 | EM Pulser T1 (strong vs shield) | PG/CPU heavy |
| Mid ×3 | Shield Booster, Web, Afterburner | |
| Low ×3 | Damage Amp, Resist Hardener (thermal), Tracking Enhancer | |

---

## 6. Example balanced 8-ship fleet (the "should-win" comp)

| # | Ship | Role | Job |
| --- | --- | --- | --- |
| 1 | Light | Scout/tackle | hold primary in place |
| 2 | Light | Scout/tackle | catch runners / EWAR escort |
| 3–4 | Medium | Fighter | focus-fire DPS (damage type per scout intel) |
| 5 | Heavy | Anchor | tank, soak alpha |
| 6 | Medium | Logistics | rep the anchor & tackle |
| 7 | Medium | EWAR | jam enemy logi/EWAR |
| 8 | Medium | Harvester/Hauler | field economy (swap to 2nd DPS for pure war) |
+ Mothership-base (capital fire support; retreats at low HP).

**Balance test (ERHeadless):** this comp vs 8× Medium Fighter of equal build cost —
the mixed comp should win via logi sustain + EWAR + tackle, validating Goal #1.

---

## 7. Balance levers & test matrix

| Lever | Effect | Risk if mis-tuned |
| --- | --- | --- |
| Resist spreads | sharpen/soften damage-type counters | flat = "bring anything" |
| Tracking vs sig | size rock-paper-scissors | big guns dominate small ships |
| Logi rep/sec | fight duration & "kill the logi" loop | brick fights or instant blobs |
| EWAR strength/duration | force-multiplier value | jam-locks (no counterplay) |
| Fitting PG/CPU | specialization pressure | do-everything ships |
| Insurance payout | risk tolerance (§13.9) | risk-free or no-fights |
| Fleet cap (6–12) | entity count (App. B / R16) & readability | bandwidth blow-up |

**Automated balance gates (ERHeadless):** for each matchup below, neither side should
win >65% over N sims; otherwise flag for tuning.
- mixed comp vs mono-DPS (equal cost) → mixed wins (intended), but ≤80%.
- right-damage-type T1 vs wrong-damage-type T2 (equal cost) → T1 competitive (≥45%).
- swarm-of-Light vs single Heavy (equal cost) → near 50/50.
- with-logi vs without-logi (equal ships) → with-logi wins, ≤70%.

---

## 8. Open questions (track)
- Exact resist % per layer/type (start ±40/±25/0, tune).
- Should the base mount offensive weapons or be fire-support/utility only? (§13.1)
- Active abilities (overheat/EWAR burst) — post-launch, gated by prediction (§13.2).
- Explosive as a 4th damage type — when/whether to introduce.

> See also: `tech-tree.md` (what unlocks these hulls/modules) and
> `economy-crafting.md` (what they cost to build).
