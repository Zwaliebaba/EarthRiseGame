# NeuronTools

Asset-pipeline parsers and build-time validators for EarthRise (masterplan
§12, §12.5, §12.6; M2 implementation plan **area A**).

Direct3D ships no model loader and DirectXTK is barred, so EarthRise parses its
own assets. The **parsing logic is platform-independent** and lives here as
dependency-free headers so a single implementation drives both:

- the **Windows runtime loaders** — `DdsLoader`/`CmoLoader`/`FontAtlas` in
  NeuronRender and `WavReader` in NeuronAudio wrap these cores (adding D3D12 /
  XAudio2 upload), and
- the **build-time `*check` tools** (`ddscheck`, `wavcheck`, `meshcook`,
  `fontpack`, `datacook`, `datacheck`) that validate/cook assets in CI.

Keeping the core dependency-free also lets it be exercised on **Linux CI**
without a Windows build (masterplan §16.2).

## Parser headers

| Header | Parses | Used by | Spec |
| --- | --- | --- | --- |
| `WavParse.h` | RIFF/WAVE → PCM-16 (mono/stereo); rejects non-PCM/non-16-bit | NeuronAudio `WavReader`, `wavcheck` | §11.3, §12.5; `neuronaudio-api.md` §3 |
| `DdsParse.h` | `DDS_HEADER`(+`DXT10`) → `DxgiFormat` (BC1–BC7, 32-bit BGRA/RGBA), mips, data offset | NeuronRender `DdsLoader`, `ddscheck` | §12.1; `neuronrender-architecture.md` §9; `darwinia-menu-ui.md` §2.1 |
| `CmoParse.h` | CMO structure → mesh/material/submesh/index/vertex counts (static meshes; skinning walked for bounds only) | NeuronRender `CmoLoader`, `meshcook` | §12.3; `neuronrender-architecture.md` §9 |
| `FontAtlasLayout.h` | fixed-grid monospace codepoint→cell→UV (EditorFont: 256×224, 16×14, cp 32) | NeuronRender `FontAtlas`, `fontpack` | §12.2; `darwinia-menu-ui.md` §2.2 |

All parsers are bounds-checked and reject malformed input with a status enum
rather than crashing.

## testrunner/

A minimal, dependency-free test harness (`TestRunner.h`) plus parser test
suites that mirror the cases in the Windows MSTest projects
(`NeuronRenderTest` / `NeuronAudioTest`). It builds and runs on any platform:

```sh
cd NeuronTools/testrunner
make          # build + run
make clean
```

CI runs this so parser regressions are caught without a Windows toolchain.

## Status

- **Done:** platform-independent parser cores (WAV/DDS/CMO/font) + Linux
  `testrunner` (30 cases passing, `-Werror` clean).
- **Next (needs the Windows build):** the runtime loaders that wrap these cores
  in NeuronRender/NeuronAudio, the `*check`/cook tool executables and their
  `.vcxproj` wiring, and the matching MSTest cases.
