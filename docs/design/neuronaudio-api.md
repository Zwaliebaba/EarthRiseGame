# EarthRise — NeuronAudio API / Class Layout

> Companion to `masterplan.md` §11.3 (audio), §12.5 (WAV assets). **Status:** DRAFT
> v0.1 — public-API sketch for review. **MS-only:** XAudio2 (2.9) + X3DAudio +
> DirectXMath; custom RIFF reader; no third-party libraries. C++23, MSVC.
>
> **Scope rules (from the plan):** client-only library; **ERHeadless links none**;
> links **NeuronCore** (math/types) but **not** NeuronRender; **WAV/PCM-16 only**;
> **3D from day one**; buses **Master→Music/Ambient/SFX/UI**; **no audio on the wire**
> (event sounds are client-side feedback off replicated sim state); presentation-only
> (no determinism requirement).

---

## 1. Layout & dependencies

```
NeuronAudio/                 [static lib, client-only]
├── engine/    AudioEngine, VoicePool, AudioDevice (IXAudio2 lifetime, suspend/resume)
├── mixer/     SubmixBus, Bus enum, MixSnapshot (per-bus + master volume)
├── wav/       WavReader (RIFF/WAVE→PCM16), WavClip (in-memory), WavStream (buffer-queue)
├── spatial/   Listener, Emitter, Spatializer (X3DAudio wrapper)
└── cue/        CueCatalog, CueId, EventBinding (sim-event → cue, data-driven)

depends on:  NeuronCore (math: Vec3=XMFLOAT3, handles, time)  +  Windows SDK
                                                                 (xaudio2.h, x3daudio.h)
used by:     EarthRise.Client (UWP)        NOT linked by: ERHeadless / NeuronClient
tested by:   NeuronAudioTest (+ WAV-parser logic also in NeuronTools/testrunner)
```

**Namespace:** `er::audio`. **Error model:** construction/init returns `HRESULT`
(thin `AU_RETURN_IF_FAILED`); per-frame calls are `noexcept` and never allocate.

---

## 2. Core value types

```cpp
namespace er::audio {

using Vec3 = DirectX::XMFLOAT3;          // camera-relative metres (floating origin §6.4)

enum class Bus : uint8_t { Music, Ambient, Sfx, Ui, Count };   // submixes under master

// Stable, generation-checked handles (mirror NeuronCore ECS handle style §7.2).
struct ClipId  { uint32_t v = 0; };      // a loaded WavClip in the AudioLibrary
struct CueId   { uint32_t v = 0; };      // a named gameplay sound cue (data-driven)
struct VoiceId { uint32_t v = 0; };      // a currently-playing instance (index+gen)

struct PlayParams {
    Bus     bus        = Bus::Sfx;
    float   volume     = 1.0f;           // linear, pre-bus
    float   pitch      = 1.0f;           // frequency ratio (1 = native)
    bool    loop       = false;
    // 3D: if set, the voice is spatialised against the listener each Update().
    const Emitter* emitter = nullptr;    // nullptr ⇒ non-positional (2D: UI/music/ambient bed)
};

struct AudioStats {                      // for §21 telemetry / NeuronAudioTest
    uint32_t activeVoices, pooledVoices, streamingVoices;
    uint32_t starvedBuffers;             // streaming underruns (should stay 0)
};

} // namespace er::audio
```

---

## 3. WAV loading — `wav/` (PCM-16 only)

```cpp
// Pure parser: no XAudio2 dependency ⇒ Linux-testable in NeuronTools/testrunner.
struct WavFormat { uint16_t channels; uint32_t sampleRate; uint16_t bitsPerSample; }; // bits==16

class WavReader {
public:
    // Parse RIFF/WAVE 'fmt '+'data'. Rejects non-PCM / non-16-bit (returns E_INVALIDARG).
    static HRESULT Parse(std::span<const std::byte> file,
                         WavFormat& outFmt, std::span<const std::byte>& outPcm) noexcept;
    static bool    IsMono(const WavFormat& f) noexcept { return f.channels == 1; }
};

// Fully-decoded short sound held in memory (event SFX / UI).
class WavClip {
public:
    static HRESULT Load(std::span<const std::byte> file, WavClip& out);
    const WavFormat& Format() const noexcept;
    std::span<const std::byte> Pcm() const noexcept;       // 16-bit PCM frames
};

// Long sound (music/ambient bed) streamed from a file/blob via buffer-queue callbacks.
class WavStream {
public:
    static HRESULT Open(std::unique_ptr<IByteSource> src, WavStream& out); // src = file/pack
    HRESULT NextChunk(std::span<std::byte> dst, uint32_t& written, bool& eof) noexcept;
    const WavFormat& Format() const noexcept;
};
```

> `wavcheck` (NeuronTools) runs `WavReader::Parse` at build time: flags non-PCM/non-16
> files and warns when a **3D-tagged cue** points at a **stereo** clip (X3DAudio needs
> mono). See §12.5.

---

## 4. Mixer — `mixer/` (the four buses)

```cpp
class SubmixBus {                         // wraps IXAudio2SubmixVoice
public:
    void  SetVolume(float linear) noexcept;     // 0..1+ ; ramped to avoid clicks
    float Volume() const noexcept;
    IXAudio2SubmixVoice* Voice() const noexcept; // routing target for source voices
};

// Master + 4 buses. Volumes are user settings (persisted client-side, not in SQL).
struct MixSnapshot { float master, music, ambient, sfx, ui; };
```

Graph: `source voices → SubmixBus[bus] → mastering voice → device`. Each bus is one
`IXAudio2SubmixVoice`; `Bus::Count` of them are created at init.

---

## 5. Spatial audio — `spatial/` (X3DAudio, 3D from day one)

```cpp
struct Listener {                         // driven by the scene camera each frame
    Vec3 position{};                      // camera-relative origin ⇒ usually {0,0,0}
    Vec3 forward{0,0,1}, up{0,1,0};
    Vec3 velocity{};                      // for Doppler
};

struct Emitter {
    Vec3  position{};                     // camera-relative metres (NO int64 — R2)
    Vec3  velocity{};                     // for Doppler
    float minDistance = 25.0f;            // full volume within
    float maxDistance = 5000.0f;          // inaudible beyond (curve in between)
    bool  doppler     = true;
};

class Spatializer {                       // thin X3DAudio wrapper
public:
    HRESULT Init(uint32_t outputChannels) noexcept;   // X3DAudioInitialize
    void    SetListener(const Listener&) noexcept;
    // Compute matrix/Doppler/LPF for one emitter and apply to its source voice.
    void    Apply(const Emitter&, IXAudio2SourceVoice*) noexcept;  // SetOutputMatrix/FreqRatio/filter
};
```

> Listener and emitters share the renderer's **floating-origin** space (§6.4), so the
> audio path never sees `int64` — the same rule as the GPU (R2).

---

## 6. The engine — `engine/` (the front door)

```cpp
class AudioEngine {
public:
    HRESULT Initialize();                 // create IXAudio2, mastering voice, 4 buses, X3DAudio
    void    Shutdown() noexcept;

    // ---- asset registry ----
    HRESULT LoadClip(std::span<const std::byte> wav, ClipId& out);  // in-memory (SFX/UI)
    HRESULT OpenMusic(std::unique_ptr<IByteSource> src, ClipId& out); // streamed

    // ---- playback ----
    VoiceId Play(ClipId, const PlayParams&) noexcept;    // returns {} if pool exhausted
    void    Stop(VoiceId) noexcept;
    void    SetVoiceVolume(VoiceId, float) noexcept;
    void    SetVoicePitch (VoiceId, float) noexcept;
    bool    IsPlaying(VoiceId) const noexcept;

    // ---- ambient bed crossfade (Bus::Ambient) ----
    void    SetAmbientBed(ClipId, float fadeSeconds) noexcept;  // cross-fades to a new bed

    // ---- mixing & spatial, called once per frame ----
    void    SetListener(const Listener&) noexcept;
    void    SetMix(const MixSnapshot&) noexcept;
    void    Update(double dtSeconds) noexcept;  // advance fades, re-spatialise 3D voices,
                                                // pump streams, reclaim finished voices

    // ---- UWP lifecycle (§11.3) ----
    void    Suspend() noexcept;                 // IXAudio2::StopEngine + release voices
    void    Resume()  noexcept;                 // StartEngine + reacquire

    AudioStats Stats() const noexcept;          // §21 telemetry
};
```

`Play` pulls a free `IXAudio2SourceVoice` from a **`VoicePool`** (keyed by
`WAVEFORMATEX` so format-compatible voices are reused), submits the clip buffer, routes
it to `bus`, and — if `emitter` is set — registers it for per-frame spatialisation.
Finished voices are reclaimed in `Update()` (via an `IXAudio2VoiceCallback` flag, not a
blocking wait).

---

## 7. Event sounds — `cue/` (data-driven, no audio on the wire)

Gameplay never sends audio. The client maps **observed sim events** (from
NeuronClient's replica/interp) to **named cues** via a data-driven catalog (game data,
versioned with the build — same boundary as balance/recipes, *not* SQL).

```cpp
enum class GameSoundEvent : uint16_t {     // raised by client systems, not the server
    WeaponFire, ProjectileImpact, ShieldHit, ArmorHit, HullHit, ShipExplode,
    ThrusterLoopStart, ThrusterLoopStop, MiningLoop, BuildComplete, LootPickup,
    BaseLowHullAlert, BaseRetreat, UiClick, UiError, /* … */
};

struct CueDef {                            // loaded from assets/audio/cues.* (game data)
    CueId    id;
    Bus      bus;
    ClipId   clip;          // or a random pick from a small variation set
    float    baseVolume, basePitch, pitchJitter;
    bool     is3d;          // ⇒ requires a mono clip (wavcheck enforces)
    bool     loop;
};

class CueCatalog {
public:
    HRESULT Load(std::span<const std::byte> cueTable, AudioEngine&); // resolves clips
    CueId   Find(std::string_view name) const noexcept;
};

// Bridges sim events → cues. Owned by the client; fed positions in camera-relative space.
class AudioEventRouter {
public:
    void Bind(GameSoundEvent, CueId) noexcept;
    VoiceId Trigger(GameSoundEvent, const Emitter* at = nullptr) noexcept; // at=nullptr ⇒ 2D
};
```

---

## 8. Usage sketches

```cpp
// --- init (UWP client startup) ---
AudioEngine audio; audio.Initialize();
ClipId pew, boom, bed, theme;
audio.LoadClip(asset("sfx/railgun.wav"), pew);     // mono → 3D-capable
audio.LoadClip(asset("sfx/explosion.wav"), boom);  // mono
audio.LoadClip(asset("amb/nebula.wav"), bed);      // stereo bed
audio.OpenMusic(file("music/earthrise_theme.wav"), theme);
audio.SetAmbientBed(bed, 2.0f);
audio.Play(theme, { .bus = Bus::Music, .loop = true });

// --- per frame ---
Listener l{ .forward = cam.Forward(), .up = cam.Up(), .velocity = cam.Velocity() };
audio.SetListener(l);
audio.Update(dt);

// --- a 3D weapon shot (camera-relative position from the replicated firing ship) ---
Emitter e{ .position = toCameraRelative(ship.universePos), .velocity = ship.vel };
audio.Play(pew, { .bus = Bus::Sfx, .pitch = jitter(0.95f,1.05f), .emitter = &e });

// --- a 2D UI click ---
audio.Play(uiClickClip, { .bus = Bus::Ui });

// --- event-driven via the router (preferred for gameplay) ---
router.Trigger(GameSoundEvent::ShipExplode, &e);
router.Trigger(GameSoundEvent::BaseLowHullAlert);   // 2D warning

// --- UWP suspend/resume ---
void OnSuspending() { audio.Suspend(); }
void OnResuming()   { audio.Resume();  }
```

---

## 9. Threading & lifetime
- XAudio2 owns its own audio thread. The client calls `Play/Stop/Set*` and `Update()`
  from the **game/render thread**; `Update()` is the only place 3D params are pushed
  and finished voices reclaimed. No locks on the audio callback path.
- `Update()` is **allocation-free** (voice reuse via `VoicePool`); clip/stream loading
  happens off the frame path at load time.
- Suspend/Resume map to `IXAudio2::StopEngine/StartEngine` + voice teardown/rebuild.

---

## 10. Testing — `NeuronAudioTest` (+ testrunner)
- **WavReader** (logic-only, Linux-testable in `NeuronTools/testrunner/`): valid PCM-16
  mono/stereo; **reject** non-PCM, 8/24/32-bit, truncated/garbage RIFF, missing
  `data`; odd chunk padding; huge/zero sizes.
- **Mixer/bus logic:** volume ramp math, `MixSnapshot` application, ambient crossfade
  curve.
- **Spatializer math:** distance attenuation curve, Doppler ratio, listener/emitter
  transforms (deterministic, mockable; no device needed).
- **VoicePool:** reuse/exhaustion, generation-checked `VoiceId` staleness.
- **Device-dependent (Windows agent):** `AudioEngine::Initialize/Shutdown`,
  suspend/resume, a smoke "play 1 clip" against a null/loopback endpoint.
- **`wavcheck`:** asserts the `is3d ⇒ mono` rule across the cue catalog.

---

## 11. Open questions (track)
- **Voice budget:** max concurrent source voices (pool size) vs CPU at battle scale —
  tie to App. B entity counts (a 20-fleet brawl could request many SFX/frame → need
  per-cue throttling/priority + distance culling of 3D voices).
- **WAV-ADPCM:** stay PCM-16 only, or allow XAudio2-native ADPCM for smaller files?
  (Currently PCM-16 only per §11.3.)
- **Doppler on/off** as a gameplay/accessibility toggle (classic "silent space" feel).
- **Reverb/filter:** use XAudio2's built-in reverb (`XAudio2CreateReverb`) per bus, or
  keep dry for the minimalist Darwinia aesthetic? (Lean dry first.)
- **Cue catalog format:** reuse the game-data serde (§7.2) or a small text table.

> See also: `masterplan.md` §11.3 / §12.5 and `combat-balance.md` (which events fire).
