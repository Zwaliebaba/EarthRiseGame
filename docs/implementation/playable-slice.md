# Playable Vertical Slice (Implementation Plan)

> **Status:** 🔨 Logic complete (camera, onboarding, selection, in-world feedback) and
> Linux-tested; **pending a Windows build + smoke run** to confirm the client wiring.
> Develops on `claude/inspiring-lovelace-gxvs5o`.
> **Relation to the masterplan:** this is a **forward-pulled slice of M7**
> ("onboarding + full UI suite", §17) plus basic RTS client affordances (§23.1/§23.2).
> It is **subordinate to [`../masterplan.md`](../masterplan.md)** and does not re-decide
> anything; it exists to make the **already-networked M3 core loop** actually *playable
> and playtestable* without waiting for M4 (scale) → M5 (persistence) → M6 (combat) → M7.

## Why

Playtesting after M3 showed the live universe rendering correctly but reading as a
**tech demo, not a game**: the camera was a fixed look-at, units could only be
"select-all"'d, and nothing told the player what to do. The networking, server-
authoritative loop, fleet commands, harvest→build, warp/jump and a clearable NPC site
**already exist** (M3 areas A–H). This slice adds the thin client layer that turns that
skeleton into something a person can sit down and play.

## Scope

In scope — **client-side**, mostly `NeuronClient` (platform-independent, Linux-tested)
+ thin `EarthRise/App.cpp` wiring (UWP/D3D, **built on the Windows agent, not in Linux CI**):

| Piece | Module (tested) | App wiring | Status |
| --- | --- | --- | --- |
| **Free RTS camera** (orbit / zoom / pan, base-follow) | `NeuronClient/RtsCamera.h` | right-drag orbit, wheel zoom, arrows pan, F follow, Space recenter | ✅ |
| **Onboarding objective chain** (Welcome→Select→Engage→Clear→Done) | `NeuronClient/Onboarding.h` | amber objective banner; observed from the replica each frame | ✅ |
| **Viewport click / box selection** | `NeuronClient/Picking.h` | left-click nearest, drag-box, live rectangle; ignores radar/UI | ✅ |
| **In-world feedback** (selection brackets, health bars, IFF colour) | `NeuronClient/HudOverlay.h` | green bracket under selected units; HP bar over combat units (own = green, hostile = red) | ✅ |

Out of scope (stays on the roadmap): real combat model (M6), persistence/identity
(M5), economy/conquest/markets and the full UI suite (M7), scale (M4). The slice does
**not** change server/sim behaviour — it only improves how the existing loop is driven
and presented.

## Verification

- The **logic** of each piece (camera math, objective state machine, pick decision)
  ships with `NeuronTools/testrunner` tests and is green on the Linux runner
  (`CameraTests`, `OnboardingTests`, `PickingTests`, `HudOverlayTests`).
- The **`App.cpp` wiring** (UWP pointer/keyboard, the CPU world→screen projection for
  picking) is **Windows-only and currently unverified** — it must be built and smoke-
  run on the Windows agent. The one thing to confirm by eye there: viewport picks line
  up with rendered unit positions (the projection mirrors the renderer's column-major
  cbuffer convention).

## How to test (Windows smoke run)

Build `EarthRise.slnx` (x64) on the Windows agent, start `ERServer`, then deploy/run the
`EarthRise` client from Visual Studio (loopback). Controls:

| Input | Expected |
| --- | --- |
| Right-drag | Camera orbits around the focus |
| Mouse wheel | Camera zooms (clamped) |
| Arrow keys | Camera pans; base-follow turns off |
| `F` / `Space` | Toggle base-follow / recenter on your base |
| Left-click a unit | Selects it (green bracket appears) |
| Left-drag a box | Selects all own units inside it (live rectangle while dragging) |
| Radar click (lower-left) | Sends the selection a smart command |
| `A` / `B` / `S` | Select all own ships / build at base / stop |

**Checklist to confirm (the App.cpp wiring the Linux tests can't cover):**

- [ ] Solution **compiles** on MSVC (UWP/WinRT + DirectXMath wiring).
- [ ] Camera orbit/zoom/pan/follow all respond; `Space` recenters on the base.
- [ ] **Selection picks line up** with rendered units — click a ship, the bracket lands on
      it; box-select grabs the right ones. *(If offset, it's the world→screen projection —
      an isolated fix; the pick decision itself is unit-tested.)*
- [ ] Health bars sit above combat units, fill matches HP, own = green / hostile = red.
- [ ] The amber **objective banner** advances Welcome → Select → Engage → Clear → Done as
      you select a fleet and clear the guardian site.

## Done (slice gate)

- [ ] A new player connects, sees the universe, can **look around** (orbit/zoom/pan),
      **select** units by clicking/box-dragging, follows an **objective prompt**, and
      can drive the existing harvest→build and **clear-the-site** loop to completion —
      with selection/health feedback visible in-world.
- [x] Camera / onboarding / selection / in-world-feedback logic implemented + Linux-tested.
- [x] In-world feedback (selection brackets + health bars) implemented.
- [ ] Built and smoke-run on the Windows agent (the real "playable" confirmation).
