# EarthRise — Game Design Docs

Companion design documents to `masterplan.md` §13 (gameplay). These are **living
balance/design drafts**, tuned against ERHeadless bot fights — not final values.

| Doc | Covers | Masterplan ref |
| --- | --- | --- |
| [`combat-balance.md`](combat-balance.md) | Damage model, defense layers, hull classes, **ship-role spreadsheet**, fitting, example fleets, balance test matrix | §13.1, §13.2 |
| [`tech-tree.md`](tech-tree.md) | Hybrid research progression: 5 branches × T1–T3, prereqs, catch-up math, the months-long player arc | §13.3 |
| [`economy-crafting.md`](economy-crafting.md) | Raw→refined→components→products **dependency graph**, markets, currency sinks, worked rebuild example | §13.4 |
| [`universe-worldgen.md`](universe-worldgen.md) | **Universe content & generation** — content layers, entity inventory, the `datacook` data schemas (region/security layout, **jump-beacon graph**, resource fields, anomaly/invasion defs), the seed-procedural model, and where stars/asteroids/jumpgates live | §6, §8.4, §12.6, §13.5–13.7, §13.11–13.12 |
| [`neuronaudio-api.md`](neuronaudio-api.md) | **NeuronAudio public API / class layout** — XAudio2/X3DAudio engine, mixer buses, WAV reader, 3D emitters, data-driven event cues | §11.3, §12.5 |
| [`touch-controls.md`](touch-controls.md) | **Touch control scheme** (EVE-Echoes-style overview-driven model) — gesture table, on-screen bars, smart-action resolution | §23, §22.3 |
| [`ui-hud-layout.md`](ui-hud-layout.md) | **HUD layout & screen flow** — in-space HUD wireframe (anchor zones), screen-flow map, per-screen layout notes | §22, §23 |
| [`neuronrender-architecture.md`](neuronrender-architecture.md) | **NeuronRender DX12 architecture** — device/frame model, descriptor & binding model, resources/PSO, HDR forward pass graph, particles, Canvas, class layout | §11, §12.4 |
| [`darwinia-menu-ui.md`](darwinia-menu-ui.md) | **Darwinia windowed menu/options UI** — reusable Canvas window toolkit (Window/Button/DropDown/Label), InterfaceGrey/InterfaceRed skin model, EditorFont atlas metrics, per-window layout, input + string-table wiring, M2 work breakdown | §11, §22.6, §22.4 |

These docs are mutually consistent: damage types (K/T/E), ship roles, hull tiers,
tiered-security scarcity, and the 6–12 fleet cap match across docs and the masterplan.

## Reviews

Architecture/code reviews live under [`review/`](review/) to keep the design list above
clean. Reviews assess existing design + code and feed decisions **back into the
masterplan** (a review's accepted recommendations are folded into `masterplan.md` and the
review is kept as the record).

| Review | Covers | Folded into |
| --- | --- | --- |
| [`review/networking-scale-review.md`](review/networking-scale-review.md) | **MMO networking review for scaling to hundreds of players** — the §8/§9 design + current M1a code; the three scaling walls (per-client snapshot pipeline, single-thread sim, single-sector pileup), the eviction self-healing fix, time dilation, delta encoding, and prioritized recommendations toward M4 | masterplan **v0.15** (§7.2, §8.2, §8.4, §8.5, §9, §17, App. A–B, R23/R24) |
