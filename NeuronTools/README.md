# NeuronTools

Build-time asset tooling and a Linux test harness for EarthRise (masterplan
§12.5, §12.6, §16.2).

> **`NeuronTools` is intentionally a leaf with no dependents.** Nothing in the
> solution links or includes anything from here; `testrunner` reaches *into* the
> libraries for the parser headers, never the other way around. The goal is to
> remove `NeuronTools` entirely once asset checking runs natively on Windows.

## Where the parsers live

The platform-independent parsing **cores** live in their **owning** libraries,
not here — so the runtime loaders and any build-time validators share one
implementation:

| Header | Owning library | Used by |
| --- | --- | --- |
| `WavParse.h` | `NeuronAudio/` | `WavReader` (NeuronAudio), future `wavcheck` |
| `DdsParse.h` | `NeuronRender/` | future `DdsLoader` (NeuronRender), `ddscheck` |
| `CmoParse.h` | `NeuronRender/` | future `CmoLoader` (NeuronRender), `meshcook` |
| `FontAtlasLayout.h` | `NeuronRender/` | future `FontAtlas` (NeuronRender), `fontpack` |

Each parser is bounds-checked and rejects malformed input with a status enum
rather than crashing.

## testrunner/

A minimal, dependency-free test harness (`TestRunner.h`) plus parser test
suites that mirror the cases in the Windows MSTest projects
(`NeuronRenderTest` / `NeuronAudioTest`). It `#include`s the parser headers from
their owning libraries (e.g. `../../NeuronAudio/WavParse.h`) and builds on any
platform:

```sh
cd NeuronTools/testrunner
make          # build + run
make clean
```

CI runs this so parser regressions are caught without a Windows toolchain.

## datacook/ — universe data tooling (§12.6)

`datacook` and `datacheck` cook and validate the **universe layout** game data
(regions/security tiers, the jump-beacon graph, resource fields — schema in
[`docs/design/universe-worldgen.md`](../docs/design/universe-worldgen.md) §4).
The runtime model, binary codec, and the referential-integrity rules live in
`NeuronCore/UniverseData.h` (shared by server/client/bots, no drift); the text
parser (`UniverseSource.h`) is build-time only and lives here. Authored sources
are under [`Config/universe/`](../Config/universe).

```sh
cd NeuronTools/datacook
make                                    # build datacook + datacheck (Linux)
make check                              # validate Config/universe/*.universe (CI gate)
./datacook  in.universe  out.bin        # text → packed binary (loaded by NeuronCore)
./datacheck in.universe                 # referential integrity only (exit 1 on error)
```

`datacheck` enforces: every beacon/field resolves its region; jump links resolve
and are **reciprocal**; the public-beacon subgraph is connected (no islands);
claimable beacons only in low/null; resource weights sum to ≈ 1.0. The same rules
run inside `datacook` so a cook never emits an invalid blob. **The leaf invariant
holds:** these tools depend *on* `NeuronCore`, never the reverse.

## Status

- **Done:** parser cores (WAV in NeuronAudio; DDS/CMO/font in NeuronRender) +
  Linux `testrunner` (65 cases passing, `-Werror` clean). `WavParse.h` is
  consumed by NeuronAudio's `WavReader`. **`datacook`/`datacheck`** (universe
  data) build and run on Linux; their parse/validate/round-trip logic is covered
  by `UniverseDataTests`.
- **Planned build-time tools (Windows):** `wavcheck`, `ddscheck`, `meshcook`,
  `fontpack`, and the asset-cooker executables — to be added here (or run
  natively), still without introducing any dependency *on* `NeuronTools`.
