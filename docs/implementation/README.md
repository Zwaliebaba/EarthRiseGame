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
2. **Scoped, not exhaustive.** A plan covers exactly one milestone's *Done* gate (§17).
   Don't pre-write M3–M7; draft each from `_template.md` when it becomes next.
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
| **M3 — Core 4X loop, fleet command & navigation** | [`M3-core-4x-loop.md`](M3-core-4x-loop.md) | 🔨 **active** |
| M4 — Scale & interest | — | ⏳ |
| M5 — Accounts, auth & persistence | — | ⏳ |
| M6 — Combat model & deployment | — | ⏳ |
| M7 — Sandbox: conquest, economy, PvE, onboarding | — | ⏳ |

New milestone plan: copy [`_template.md`](_template.md) → `M<n>-<slug>.md`, fill it from the
matching masterplan §17 milestone, add a row above.
