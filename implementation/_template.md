# M&lt;n&gt; — &lt;Milestone Title&gt; (Implementation Plan)

> Derived from [`../masterplan.md`](../masterplan.md) §17 (milestone **M&lt;n&gt;**).
> **Status:** ⏳ Not started · 🔨 In progress · ✅ Complete — pick one.
> **Plan style:** feature-area sections (see `README.md`).

## Milestone goal (verbatim from §17)

> _Paste the one-paragraph M&lt;n&gt; description and its **Done:** gate from masterplan §17._

## Scope at a glance

- **In scope:** …
- **Out of scope (later milestone):** …
- **Open questions that touch this milestone** (from §19): list each, with the decision
  needed *before* the work item it blocks — or mark "deferred, doesn't block M&lt;n&gt;".

## Current state (what M&lt;n−1&gt; already left us)

_Brief, file-level: what exists, what's a placeholder this milestone replaces._

---

## Feature areas

For each area, keep the same shape:

### &lt;Letter&gt;. &lt;Feature area name&gt;

- **Goal:** one sentence.
- **Masterplan refs:** §… · **Design doc:** `docs/design/…` (if any).
- **Current state:** what exists today / what's net-new.
- **Work:**
  - [ ] item — target file/module
  - [ ] …
- **Tests (`<project>Test`, §16.1):**
  - [ ] `…Test`: case — what it asserts
- **Depends on:** &lt;other areas/items&gt; · **Blocks:** &lt;…&gt;

---

## Suggested order / dependency notes

_A short ordering of the areas above (what unblocks what). Areas with no dependency can run
in parallel._

## Done gate (mirrors §17 "Done")

- [ ] _restate each clause of the masterplan Done criteria as a checkbox_
- [ ] All matching `<project>Test` suites green (§16.1).
- [ ] Per-milestone perf gate met where applicable (§16.3, App. B).
