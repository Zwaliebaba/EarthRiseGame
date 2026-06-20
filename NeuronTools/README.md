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

## Status

- **Done:** parser cores (WAV in NeuronAudio; DDS/CMO/font in NeuronRender) +
  Linux `testrunner` (30 cases passing, `-Werror` clean). `WavParse.h` is
  consumed by NeuronAudio's `WavReader`.
- **Planned build-time tools (Windows):** `wavcheck`, `ddscheck`, `meshcook`,
  `fontpack`, `datacook`/`datacheck` — to be added here (or run natively),
  still without introducing any dependency *on* `NeuronTools`.
