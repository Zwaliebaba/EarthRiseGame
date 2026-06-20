# EarthRise — Game Design Docs

Companion design documents to `masterplan.md` §13 (gameplay). These are **living
balance/design drafts**, tuned against ERHeadless bot fights — not final values.

| Doc | Covers | Masterplan ref |
| --- | --- | --- |
| [`combat-balance.md`](combat-balance.md) | Damage model, defense layers, hull classes, **ship-role spreadsheet**, fitting, example fleets, balance test matrix | §13.1, §13.2 |
| [`tech-tree.md`](tech-tree.md) | Hybrid research progression: 5 branches × T1–T3, prereqs, catch-up math, the months-long player arc | §13.3 |
| [`economy-crafting.md`](economy-crafting.md) | Raw→refined→components→products **dependency graph**, markets, currency sinks, worked rebuild example | §13.4 |
| [`neuronaudio-api.md`](neuronaudio-api.md) | **NeuronAudio public API / class layout** — XAudio2/X3DAudio engine, mixer buses, WAV reader, 3D emitters, data-driven event cues | §11.3, §12.5 |

All three are mutually consistent: damage types (K/T/E), ship roles, hull tiers,
tiered-security scarcity, and the 6–12 fleet cap match across docs and the masterplan.
