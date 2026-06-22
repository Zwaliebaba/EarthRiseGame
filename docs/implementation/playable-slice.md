# Playable Vertical Slice (Implementation Plan)

> **Status:** 🔨 In progress — camera, onboarding and selection landed; in-world
> feedback pending. Develops on `claude/inspiring-lovelace-gxvs5o`.
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
| **In-world feedback** (selection rings, health bars, hostile markers) | — | screen-projected overlays in the HUD / scene | ⏳ next |

Out of scope (stays on the roadmap): real combat model (M6), persistence/identity
(M5), economy/conquest/markets and the full UI suite (M7), scale (M4). The slice does
**not** change server/sim behaviour — it only improves how the existing loop is driven
and presented.

## Verification

- The **logic** of each piece (camera math, objective state machine, pick decision)
  ships with `NeuronTools/testrunner` tests and is green on the Linux runner
  (`CameraTests`, `OnboardingTests`, `PickingTests`).
- The **`App.cpp` wiring** (UWP pointer/keyboard, the CPU world→screen projection for
  picking) is **Windows-only and currently unverified** — it must be built and smoke-
  run on the Windows agent. The one thing to confirm by eye there: viewport picks line
  up with rendered unit positions (the projection mirrors the renderer's column-major
  cbuffer convention).

## Done (slice gate)

- [ ] A new player connects, sees the universe, can **look around** (orbit/zoom/pan),
      **select** units by clicking/box-dragging, follows an **objective prompt**, and
      can drive the existing harvest→build and **clear-the-site** loop to completion —
      with selection/health feedback visible in-world.
- [x] Camera / onboarding / selection logic implemented + Linux-tested.
- [ ] In-world feedback (selection rings + health bars) implemented.
- [ ] Built and smoke-run on the Windows agent (the real "playable" confirmation).
