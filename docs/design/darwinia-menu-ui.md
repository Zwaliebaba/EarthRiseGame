# EarthRise — Darwinia Menu / Window UI (Design + Implementation Plan)

> Companion to [`../masterplan.md`](../masterplan.md) §11 (Canvas), §22 (UI/HUD),
> §22.4 (localization string table), §22.5 (HUD scale), §12.1 (DDS), §12.2 (fonts),
> §25 (settings). **Folded into milestone M2** — see
> [`../implementation/M2-darwinia-audio.md`](../implementation/M2-darwinia-audio.md)
> areas **F** (Canvas/widgets) and **G** (settings screen).
> **Status:** DRAFT v0.1 — implementation plan for review.
>
> **Purpose:** reproduce **exactly the windowed menu canvas in the reference
> screenshot** (the *Options*, *Main Menu*, *Screen / Graphics / Other Options*
> windows) as a **reusable, interactive immediate-mode widget toolkit** built on the
> existing `CanvasRenderer`, using the supplied `InterfaceGrey.dds` / `InterfaceRed.dds`
> chrome textures and the `EditorFont` bitmap atlas. Strings are **EarthRise-themed and
> routed through the §22.4 string table** (no hard-coded display text); the *look,
> widget set, and layout* match the screenshot 1:1.

---

## 0. Decisions baked into this plan

| # | Decision | Source |
| --- | --- | --- |
| D1 | **Interactive toolkit**, not a static mock. Windows are draggable/closable; buttons hover/press; dropdowns open and select. The 5 windows shown are the working demo + visual-acceptance target. | user |
| D2 | **EarthRise strings via the §22.4 string table.** Same widgets/layout/counts as the screenshot; Darwinia-specific labels become EarthRise equivalents (e.g. *Leave Darwinia → Quit*, the build-version string becomes EarthRise's). | user |
| D3 | **Fold into M2** (the "Darwinia look + monospace Canvas HUD + settings screen" milestone). This doc is referenced from masterplan §22.6 and the M2 plan. | user |
| D4 | Canvas stays a **primitive layer**; the **widget toolkit logic** (hit-test, layout, skin-UV math) lives in **NeuronRender** so `NeuronRenderTest` (§16.1) covers it; the **screen definitions + pointer plumbing** live in the **EarthRise** UWP client. | repo conventions (§22, agents.md, §16.1) |
| D5 | **No new code subdirectories** — new files sit flat in their project and are grouped with Visual Studio **Filters**. | `agents.md` |

---

## 1. The reference canvas (what we are reproducing)

Five cascading windows, all sharing one chrome style:

```
 ┌─ window anatomy ────────────────────────────┐
 │▓▓▓▓▓▓▓▓ TITLE (centred) ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓ [x] │ ← title bar  : InterfaceGrey skin, dark title text, close box top-right
 ├─────────────────────────────────────────────┤
 │                                             │ ← body       : dark translucent red-black fill + 1px bevel border
 │  Label text          [ value        ▼ ]     │ ← DropDown   : label (left, cream) + value box (red skin) + ▼ arrow
 │  Label text          [ value        ▼ ]     │
 │                                             │
 │  ▏▏▏▏▏▏▏▏  Button caption  ▏▏▏▏▏▏▏▏          │ ← Button     : InterfaceRed skin, cream caption; highlight = grey "beam"
 │                                             │
 │            165 FPS   (plain Label)          │ ← Label      : monospace text, no chrome
 │   [   Close   ]            [   Apply   ]    │ ← footer buttons
 └─────────────────────────────────────────────┘
```

| Window (screenshot title) | Contents (count fixed for fidelity) | Widget kinds |
| --- | --- | --- |
| **OPTIONS** | Screen / Graphics / Sound / Control / Other Options, **Close** | 6 × Button (vertical list) |
| **MAIN MENU** | Profile, Mods, Options, Visit Website, Play Prologue, *Leave* | 6 × Button |
| **SCREEN OPTIONS** | 13 rows: Resolution … FXAA, **Close / Apply** | 13 × DropDown + 2 × Button |
| **GRAPHICS OPTIONS** | 7 rows: Field of view … Pixel Effect, **FPS** label, **Close / Apply** | 7 × DropDown + 1 × Label + 2 × Button |
| **OTHER OPTIONS** | 7 rows: Help System … Automatic Camera, **version** label, **Close / Apply** | 7 × DropDown + 1 × Label + 2 × Button |

So the entire screenshot is expressible with **four widget types** — **Window** (title bar + close box + body), **Button**, **DropDown**, **Label** — plus the dropdown's **expansion list** (a popup of selectable rows). Build those and the canvas reproduces exactly.

---

## 2. Asset inventory (measured from the supplied files)

All interface/font DDS are **uncompressed 32-bit BGRA** — i.e. `DXGI_FORMAT_B8G8R8A8_UNORM` (header masks `R=00ff0000 G=0000ff00 B=000000ff A=ff000000`), **mip count 1**, which **matches the swap-chain backbuffer format** already used by the Canvas PSO.

### 2.1 Chrome textures — `EarthRise/Assets/Textures/`

| File | Size | Content (measured) | Use |
| --- | --- | --- | --- |
| `InterfaceGrey.dds` | 64 × 512 | **Single vertical gradient**, dark→bright→dark, peak ≈ `(B95,G83,R83)` at mid-height; ~neutral grey, faint blue bias; subtle 2-px dither | Title bar, window border/bevel, **button highlight beam** |
| `InterfaceRed.dds` | 64 × 510 | **Single vertical gradient**, dark→bright→dark, peak ≈ `(B31,G31,R71)` at mid-height; strongly **R-dominant** | Buttons, dropdown value boxes, window body fill |

> **Key insight:** these are **not** multi-state atlases — each is one vertical "sheen"
> gradient. A widget is a quad textured with the strip so its **V axis maps across the
> widget height** (giving the glossy top-bright / bottom-dark bevel); the **U axis is
> stretched** (the strip is horizontally near-uniform). **Material** = which strip
> (grey vs red); **state** (normal / hover / pressed / selected) = a **tint multiply**
> and/or a brighter V sub-range, applied in the pixel shader. This is the whole
> Darwinia widget-rendering trick.

### 2.2 Font atlas — `EarthRise/Assets/Fonts/`

`EditorFont-ENG.dds` (the clean monospace used by the menus). `SpeccyFont-*` is the
retro ZX variant — same dimensions/layout, reserved for in-world retro text.

| Property | Value (measured & verified) |
| --- | --- |
| Atlas size | **256 × 224**, BGRA, white glyphs with **alpha coverage on a transparent background** |
| Grid | **16 columns × 14 rows = 224 cells**, **cell = 16 × 16 px** (separator-row analysis confirms a 16-px pitch both axes) |
| First codepoint | **32 (space)** → cells cover **cp 32–255** row-major; space (cp 32) verified blank |
| Glyph→cell | `idx = cp − 32; col = idx & 15; row = idx >> 4` |
| Glyph→UV | `u0 = col·16/256, v0 = row·16/224, u1 = u0 + 16/256, v1 = v0 + 16/224` |
| Localization | language-suffixed atlases (`-ENG/-FRA/-ITA/-RUS`) select by language (§22.4); all share these metrics |

> The ▼ dropdown arrow and box-drawing chrome are **not** in a 32–255 atlas, so the
> dropdown arrow is drawn as a **small filled triangle primitive** (see §4.1), not a glyph.

---

## 3. Where the code lives (architecture)

```
NeuronRender/   (static lib; testable via NeuronRenderTest)        [files flat + VS filters]
  CanvasRenderer.{h,cpp}   EXTEND: textured + tinted quads (UV), font-atlas text, triangle prim
  shaders/CanvasVS.hlsl    EXTEND: pass-through UV + per-vertex tint
  shaders/CanvasPS.hlsl    EXTEND: sample texture (or font), multiply by tint; texture-vs-solid mode
  DeviceResources.{h,cpp}  EXTEND: shader-visible CBV/SRV heap + static sampler (none today)
  DdsLoader.{h,cpp}        NEW   : uncompressed 32-bit BGRA DDS → TextureGpu (BC path is area-A scope)
  TextureGpu.{h}           NEW   : DEFAULT-heap texture + SRV handle
  FontAtlas.{h,cpp}        NEW   : fixed-grid metrics (§2.2), codepoint→UV
  UiWidgets.{h,cpp}        NEW   : immediate-mode Window/Button/DropDown/Label + hit-test + skin/layout
  UiSkin.{h}               NEW   : skin = {texture, tint, vRange} for grey/red/states (§2.1)

EarthRise/   (UWP client app)                                      [files flat + VS filters]
  MenuScreens.{h,cpp}      NEW   : the 5 windows — positions, contents, string-ids, actions
  StringTable.{h,cpp}      NEW   : id→text (§22.4); English now, language-swappable
  UiInput.{h,cpp}          NEW   : CoreWindow pointer/keyboard → UiContext events
  App.cpp                  EXTEND: own a UiContext; pump input; draw menus each frame
```

- **Canvas remains primitive** (quads/text/lines/triangles). The **widget toolkit** is a
  thin immediate-mode layer that *emits* Canvas primitives and consumes input — its pure
  logic (hit-testing, layout stacking, skin-UV math, dropdown open/selected state) is
  data-only and unit-testable without a device.
- **NeuronClient / ERServer / ERHeadless are untouched** — this is pure presentation.

---

## 4. Rendering model

### 4.1 CanvasRenderer extension (the only renderer change)

Current Canvas vertex is `{x, y, r, g, b, a}` with a solid-color PSO. Extend to support
textures + the font without breaking the existing color path:

- **Vertex** → `{x, y, u, v, r, g, b, a}` (add `float2 uv`). Color is now a **tint**
  multiplied onto the sampled texel.
- **Two PSOs sharing one root signature** (or one PSO + a "use-texture" flag via root
  constant):
  - *Solid* (existing M1b behaviour) — `tint` only, white 1×1 texel or texture-disable flag.
  - *Textured* — sample SRV (chrome strip **or** font atlas) × `tint`.
- **Root signature** gains: a **descriptor table (SRV, t0)** for the bound texture + a
  **static sampler (s0)**, `MIN_MAG_MIP_LINEAR` for chrome (smooth gradient),
  **`POINT`** for the font (crisp pixels). Keep the existing root-constants
  (`invScreenSize`) at b0; add one root constant for the texture-mode flag.
- **New primitives on CanvasRenderer:**
  - `DrawTexturedQuad(Rect dst, Rect uv, Color tint, TextureGpu*)` — the chrome workhorse.
  - `DrawText(x, y, text, Color, scale, FontAtlas*)` — replaces the placeholder bar loop
    with real per-glyph textured quads (UV from §2.2). Monospace advance = `cellPx·scale`
    (default `scale = textPx/16`).
  - `DrawTriangle(p0, p1, p2, Color)` — the ▼ dropdown arrow (and future ticks/markers).
- **Batching:** keep the single dynamic upload-buffer batch, but **flush on texture
  change** (chrome strip → font atlas → next window). Draw order is back-to-front
  (window body → content → popups), so per-window batches stay small. `kMaxQuads`
  bumped as needed.
- **Compositing:** unchanged — Canvas runs **after** tone-map, over the backbuffer, no
  depth, alpha-blended (matches the existing PSO).

### 4.2 DDS loader (uncompressed path)

A small `DdsLoader` parses the `DDS_HEADER`, and for these assets takes the
**uncompressed 32-bit RGB+alpha** branch: validate `dwSize=124`, masks =
`A8R8G8B8` → `DXGI_FORMAT_B8G8R8A8_UNORM`, copy the single mip into a DEFAULT-heap
`Texture2D`, create an SRV. (The BC1–BC7 + multi-mip branch noted in
`neuronrender-architecture.md` §9 / M2 area A is for meshes/skybox and is **not blocked
by this work** — these UI assets need only the uncompressed path.)

### 4.3 Skin spec (starting values; final tints tuned to the screenshot)

```text
struct UiSkin { TextureGpu* tex; float vTop, vBot; Color tint; }   // V sub-range + tint
```

| Element | Texture | V range | Tint (multiply) | Notes |
| --- | --- | --- | --- | --- |
| Title bar | Grey | full sheen | bright (≈ ×1.4, slight blue) | title text dark `(20,20,28)` |
| Close box | Grey | full | as title; **hover** brighter | small square, top-right inset |
| Window body | Red | mid (dark) | dark `(0.5,0.5,0.5)` × **alpha ≈ 0.88** | translucent — scene shows faintly through |
| Window border/bevel | Grey | top & bottom edges | mid | 1–2 px frame |
| Button (normal) | Red | full sheen | ×1.0 | caption cream `(235,225,205)` |
| Button (hover) | Red | full | ×1.25 | |
| Button (pressed) | Red | shifted down (darker) | ×0.85 | |
| Button (**selected**) | Grey beam over Red | full | additive light-blue beam | the "Graphics Options" highlight |
| DropDown value box | Red | mid | ×0.9, recessed bevel | value text cream; ▼ arrow cream |
| Label (FPS / version) | — (text only) | — | — | plain monospace, no chrome |

---

## 5. Widget toolkit (immediate-mode)

A retained-data-free toolkit: a per-frame `UiContext` holds input state + a tiny amount
of persisted UI state (which window has focus / drag, which dropdown is open, window
positions). Each widget is a function that draws + returns interaction.

```cpp
struct UiContext {
  // input snapshot (from UiInput)
  Vec2 mouse; bool mouseDown, mousePressed, mouseReleased; int wheel;
  // persisted UI state
  WindowId focus, dragging; Vec2 dragGrab;
  DropDownId openDropdown; int openHoverRow;
  // services
  CanvasRenderer* canvas; FontAtlas* font; UiSkin grey, red; StringTable* strings;
};

// returns true on this-frame activation; *open/*sel are persisted by caller
bool  BeginWindow(UiContext&, WindowId, Rect& rect, StringId title, bool closable);
void  EndWindow(UiContext&);
bool  Button   (UiContext&, Rect, StringId caption, bool selected = false);
bool  DropDown (UiContext&, Rect, StringId label, std::span<const StringId> items, int& sel);
void  Label    (UiContext&, Vec2, StringId, Color, float scale = 1.f);
```

**Behaviour (interactivity, D1):**
- **Window:** title-bar drag moves `rect` (clamped on-screen); clicking a window raises it
  (focus / draw-order); **close box** returns/sets closed. Stacking order = a small
  z-list so overlapping windows match the screenshot's cascade and front-to-back picking.
- **Button:** hover/press/selected visuals per §4.3; fires on release-inside.
- **DropDown:** the value box toggles an **expansion popup** (list of `items` rows drawn
  above all windows); hovering highlights a row; clicking sets `sel` and closes; clicking
  elsewhere closes without change. Only one dropdown open at a time (`openDropdown`).
- **Hit-testing** respects z-order and **swallows** input that lands on an open popup or a
  focused window, so clicks don't fall through to windows behind.

**Layout helper:** a vertical stacker (title height, content padding, row pitch, button
size, label/value column split) so each screen is declared as data, e.g. row pitch
≈ 40 px, button height ≈ 32 px, label column ≈ 45 %, value column ≈ 45 %, gutter ≈ 10 %.

---

## 6. The five screens (exact layout)

Positions/sizes are at a **1920 × 1080 reference**, multiplied by the user **HUD scale**
(§22.5) and re-anchored on resize. Values below are measured from the screenshot and are
the **starting layout**, finalized against the reference image by the visual gate (§9).

| Window | x | y | w | h | rows | footer |
| --- | --- | --- | --- | --- | --- | --- |
| Options | 355 | 243 | 300 | 296 | 6 buttons | — |
| Main Menu | 808 | 378 | 278 | 250 | 6 buttons | — |
| Screen Options | 1188 | 116 | 576 | 640 | 13 dropdowns | Close · Apply |
| Graphics Options | 838 | 626 | 498 | 440 | 7 dropdowns + FPS label | Close · Apply |
| Other Options | 262 | 543 | 498 | 432 | 7 dropdowns + version label | Close · Apply |

Window chrome metrics: title-bar height ≈ 28 px; close box ≈ 18 px square inset 6 px from
top-right; body padding ≈ 14 px; dropdown row pitch ≈ 40 px; button height ≈ 32 px.

> The screenshot shows all five **open at once, cascaded** — reproduce that exact initial
> arrangement as the demo/acceptance scene (each window is independently movable/closable).

---

## 7. Strings (EarthRise-themed, via the §22.4 string table)

Counts and positions match the screenshot; wording is EarthRise's, by string id (English
shown). Darwinia-specific items are re-themed; generic settings keep their meaning and map
to EarthRise's real §25/§11.1 options.

| Screenshot label | String id | English (EarthRise) |
| --- | --- | --- |
| OPTIONS / *Screen/Graphics/Sound/Control/Other Options* / Close | `ui.options.*` | unchanged (generic) |
| MAIN MENU | `ui.mainmenu.title` | "MAIN MENU" |
| Profile / Mods / Options / Visit Website | `ui.mainmenu.*` | unchanged |
| Play Prologue | `ui.mainmenu.play` | "Play Tutorial" |
| **Leave Darwinia** | `ui.mainmenu.quit` | **"Quit EarthRise"** |
| SCREEN OPTIONS rows (Resolution…FXAA) | `ui.screen.*` | map to §11.1/§25 (Resolution, Window Mode, VSync, Limit FPS, Anisotropy, FXAA …) |
| GRAPHICS OPTIONS rows | `ui.graphics.*` | Field of View, then EarthRise quality knobs (Bloom, Particles, Render Scale, Shadow/Entity Detail, Pixel Effect — §11.1) |
| 165 FPS | `ui.graphics.fps` | live FPS readout (format string) |
| OTHER OPTIONS rows | `ui.other.*` | Help System, Controller Help, Intro Movies, Language, Difficulty, Large Menus (HUD scale), Automatic Camera |
| version string | `ui.build.version` | EarthRise build/version string (not the Darwinia one) |

> Exact final wording is a small content decision; the **layout, row counts, and widget
> kinds are fixed** to match the canvas. `StringTable` lookup of a missing id returns a
> visible `!id!` fallback (testable).

---

## 8. Input wiring (App.cpp)

- Subscribe `CoreWindow` `PointerPressed/Moved/Released/WheelChanged` (+ existing
  `KeyDown`); fold into a `UiInput` snapshot consumed by `UiContext` each frame **before**
  building the menus. (DPI via `DisplayInformation`, feeding HUD scale.)
- In `Tick()`, after the scene render, run the menu pass: `ui.Begin(input)` → declare the
  open windows (`MenuScreens::Draw`) → `canvas.Render(...)`. The existing
  `DrawText("EarthRise"/state)` HUD calls stay (or move into a debug overlay).
- Button/dropdown actions are local UI for now (open/close windows, change a setting
  value, Quit). Wiring settings to the real graphics/audio knobs is **M2 area G**; gameplay
  commands→intents are later milestones (§23/§8.4) — out of scope here.

---

## 9. Acceptance — "exactly the same canvas"

- [ ] All five windows render with correct **chrome** (grey title bars + red bodies/buttons),
      correct **titles/labels/values** (themed), and the screenshot's **cascade layout**.
- [ ] **Interactive:** hover/press buttons; open a dropdown and pick a value; drag a window
      by its title bar; close a window via its close box; front-to-back picking is correct.
- [ ] **Visual diff:** capture the demo scene at 1920×1080 and compare against the reference
      screenshot (ignore the 3D background, §0); iterate skin tints / metrics (§4.3, §6)
      until the menu boxes match. This is the milestone's visual gate.
- [ ] Runs within the M2 render frame-time budget (§16.3, App. B); Canvas batching keeps the
      menu pass to a handful of draws.

---

## 10. Work breakdown (M2 add-on; ordered)

Feature-area shape per `implementation/_template.md`; each item names its `<project>Test`
gate (§16.1). These extend **M2 areas F (Canvas/HUD) & G (settings)**.

### MU-1. Canvas texture + font foundation  · *NeuronRender*
- [ ] DeviceResources: shader-visible CBV/SRV heap + static sampler(s) — `DeviceResources`
- [ ] `DdsLoader` + `TextureGpu`: uncompressed 32-bit BGRA → SRV — `DdsLoader`
- [ ] `FontAtlas`: §2.2 metrics, codepoint→UV — `FontAtlas`
- [ ] CanvasRenderer: UV vertex, textured PSO, `DrawTexturedQuad` / real `DrawText` /
      `DrawTriangle`; flush-on-texture-change — `CanvasRenderer`, `CanvasVS/PS`
- **Tests (`NeuronRenderTest`):** DDS parse (valid 32-bit / truncated / wrong-mask → clean
  fail); FontAtlas UV for cp 32/65/255 + out-of-range clamp; Canvas batch builds expected
  vertex/UV for a known string; PSO builds from embedded DXIL. Mirror parser/UV cases into
  `NeuronTools/testrunner` (§16.2). **Depends on:** nothing. **Blocks:** MU-2/3.

### MU-2. Widget toolkit + skin  · *NeuronRender*
- [ ] `UiSkin` (grey/red + state tints, §4.3); `UiWidgets`: Window/Button/DropDown/Label,
      hit-test, z-order, drag, dropdown popup — `UiWidgets`, `UiSkin`
- **Tests (`NeuronRenderTest`):** hit-test (point in/out, z-order picking, popup swallow);
  layout stacker math (row rects, label/value split); dropdown open/select state machine;
  window drag-clamp. (All device-free logic.) **Depends on:** MU-1. **Blocks:** MU-3.

### MU-3. The five screens + strings + input  · *EarthRise client*
- [ ] `StringTable` (id→text, missing→`!id!`) + EarthRise entries (§7) — `StringTable`
- [ ] `MenuScreens`: declare the 5 windows (§6) with themed strings + actions — `MenuScreens`
- [ ] `UiInput`: CoreWindow pointer/wheel → snapshot; wire into `App::Tick` — `UiInput`, `App.cpp`
- [ ] Initial cascade arrangement = the screenshot; ESC/close behaviour
- **Tests:** `StringTable` round-trip + missing-id fallback (testrunner/NeuronRenderTest as
  pure logic); `MenuScreens` builds the expected widget set (counts/ids per window).
  **Depends on:** MU-1, MU-2.

### MU-4. Polish & visual gate
- [ ] Tune tints/metrics to the reference (§9 visual diff); HUD-scale + resize re-anchor
- [ ] Package assets in the appx (fonts + interface DDS); confirm load paths
- **Tests:** resize/HUD-scale layout stays within window bounds; frame-time spot-check.

**Order:** MU-1 → MU-2 → MU-3 → MU-4. MU-1 is the only renderer-plumbing change and
unblocks everything; MU-2 is pure logic (parallelizable with MU-3's StringTable).

---

## 11. Open questions
- Final EarthRise wording for re-themed items (§7) — content decision; doesn't block layout.
- Exact selected-button highlight: additive grey beam vs brighter-red (pick by visual diff).
- Whether dropdown **values** are real settings now or placeholders until M2 area G wires
  §25 settings (default: placeholders that round-trip in the toolkit; real wiring in G).
- Sampler choice per surface (linear chrome / point font) — confirm crispness on the diff.

> See also: `masterplan.md` §11 / §22 / §22.4 / §25, `neuronrender-architecture.md`
> (Canvas §8, asset loaders §9), `ui-hud-layout.md`, `implementation/M2-darwinia-audio.md`.
