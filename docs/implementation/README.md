# EarthRise — Implementation Plans

> **Purpose:** turn the design in [`../masterplan.md`](../masterplan.md) into ordered,
> testable engineering work. One plan per **active milestone**.

## How these relate to `masterplan.md`

| Document | Answers | Altitude | Lifetime |
| --- | --- | --- | --- |
| **`masterplan.md`** | *What* are we building and *why*? Locked decisions, architecture, subsystem specs, risks, milestone gates. | High (design source-of-truth). | Long-lived; versioned (v0.x changelog). |
| **`docs/design/*.md`** | *How* does one subsystem hang together? Class layouts, wireframes, API sketches. | Mid (per-subsystem). | Updated as subsystems firm up. |
| **`implementation/M*.md`** | *In what order, in which files, gated by which test* do we build the next milestone? | Low (execution). | Short-lived; archived once the milestone closes. |

**The masterplan stays authoritative on decisions.** Implementation plans never re-decide
anything — they **sequence** the work and point back to the masterplan by `§` number. If a
plan and the masterplan disagree, the masterplan wins; fix the plan (or, if a real decision
changed, bump the masterplan first, then the plan).

## Rules of the road

1. **One hot plan at a time.** Only the current milestone has a live plan. M3 is current
   (`M3-core-4x-loop.md`). Earlier milestones (M0/M1a/M1b/M2) are complete — see the
   masterplan footer.
2. **Scoped, not exhaustive.** A plan covers exactly one milestone's *Done* gate (§17). M4,
   M5 and M6 are **drafted ahead** (from §17 + the §8.4/§9/§13/§14/§15/§20/§25 specs) as
   forward-looking roadmaps; they stay subordinate to the masterplan and are **re-confirmed against
   it when each goes active** (its predecessor's actual landing may shift details). Don't draft M7
   until it's next.
3. **Every feature ships with its test in the same commit** (masterplan §16.1). Each work
   item names the `<project>Test` cases that gate it. A feature isn't done until its tests
   pass.
4. **Reference, don't duplicate.** Cite `§11.1`, `docs/design/...`, etc. rather than copying
   spec text — copies drift.
5. **Archive on close.** When a milestone's *Done* gate is met, mark the plan ✅ Complete and
   leave it as a record; start the next from the template.

## Index

| Milestone | Plan | Status |
| --- | --- | --- |
| M0 — Foundations | (complete — masterplan §17) | ✅ |
| M1a — Networked transport (headless) | (complete — masterplan §17) | ✅ |
| M1b — Client tech slice | (complete — masterplan §17) | ✅ |
| M2 — Darwinia look + audio | [`M2-darwinia-audio.md`](M2-darwinia-audio.md) | ✅ |
| **M3 — Core 4X loop, fleet command & navigation** | [`M3-core-4x-loop.md`](M3-core-4x-loop.md) | 🔨 **active** (areas A–H implemented; Windows client/headless glue unverified) |
| M4 — Scale & interest | [`M4-scale-interest.md`](M4-scale-interest.md) | 🔨 pipeline complete (areas A–J platform-independent logic implemented + tested; Windows IOCP/ERServer integration + the live wall-clock perf gate remain) |
| M5 — Accounts, auth & persistence | [`M5-accounts-persistence.md`](M5-accounts-persistence.md) | ⏳ drafted |
| M6 — Combat model & deployment | [`M6-combat-deployment.md`](M6-combat-deployment.md) | ⏳ drafted |
| M7 — Sandbox: conquest, economy, PvE, onboarding | — | ⏳ |

### Side tracks

| Track | Plan | Status |
| --- | --- | --- |
| Playable vertical slice (forward-pulled M7 onboarding + RTS client affordances) | [`playable-slice.md`](playable-slice.md) | 🔨 logic complete (camera / onboarding / selection / feedback), Linux-tested; pending Windows smoke run |

New milestone plan: copy [`_template.md`](_template.md) → `M<n>-<slug>.md`, fill it from the
matching masterplan §17 milestone, add a row above.
