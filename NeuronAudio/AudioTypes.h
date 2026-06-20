#pragma once
// NeuronAudio core value types (device-free, header-only).
//
// These mirror docs/design/neuronaudio-api.md §2 but use the codebase
// namespace convention (`Neuron::Audio`, matching `Neuron::Render`) rather than
// the doc's aspirational `er::audio`. Everything here is pure data + inline math
// so NeuronAudioTest can exercise the mixer/handle logic without an XAudio2
// device (the device-bound classes live in their own headers/.cpp).

#include <cstdint>

#include <DirectXMath.h>

namespace Neuron::Audio
{
  // Camera-relative metres (floating origin §6.4) — no int64 reaches audio (R2).
  using Vec3 = DirectX::XMFLOAT3;

  // Submix buses under the mastering voice (§11.3, locked order).
  enum class Bus : uint8_t
  {
    Music = 0,
    Ambient,
    Sfx,
    Ui,
    Count
  };

  inline constexpr uint32_t BUS_COUNT = static_cast<uint32_t>(Bus::Count);

  // Stable, generation-checked handles (mirror NeuronCore ECS handle style §7.2).
  struct ClipId
  {
    uint32_t v = 0;
    constexpr bool valid() const noexcept { return v != 0; }
    constexpr bool operator==(const ClipId&) const noexcept = default;
  };

  struct VoiceId
  {
    uint32_t v = 0; // packed index + generation (see VoicePool.h)
    constexpr bool valid() const noexcept { return v != 0; }
    constexpr bool operator==(const VoiceId&) const noexcept = default;
  };

  struct CueId
  {
    uint32_t v = 0;
    constexpr bool valid() const noexcept { return v != 0; }
    constexpr bool operator==(const CueId&) const noexcept = default;
  };

  // Listener = the scene camera, fed each frame (§11.2/§11.3).
  struct Listener
  {
    Vec3 position{0.0f, 0.0f, 0.0f}; // camera-relative ⇒ usually origin
    Vec3 forward{0.0f, 0.0f, 1.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    Vec3 velocity{0.0f, 0.0f, 0.0f}; // for Doppler
  };

  struct Emitter
  {
    Vec3 position{0.0f, 0.0f, 0.0f}; // camera-relative metres (NO int64 — R2)
    Vec3 velocity{0.0f, 0.0f, 0.0f}; // for Doppler
    float minDistance = 25.0f;       // full volume within this radius
    float maxDistance = 5000.0f;     // inaudible beyond this radius
    bool doppler = true;
  };

  struct PlayParams
  {
    Bus bus = Bus::Sfx;
    float volume = 1.0f; // linear, pre-bus
    float pitch = 1.0f;  // frequency ratio (1 = native)
    bool loop = false;
    const Emitter* emitter = nullptr; // nullptr ⇒ non-positional (2D)
  };

  // Per-bus + master volume (user settings; persisted client-side, not SQL).
  struct MixSnapshot
  {
    float master = 1.0f;
    float music = 1.0f;
    float ambient = 1.0f;
    float sfx = 1.0f;
    float ui = 1.0f;
  };

  struct AudioStats
  {
    uint32_t activeVoices = 0;
    uint32_t pooledVoices = 0;
    uint32_t streamingVoices = 0;
    uint32_t starvedBuffers = 0; // streaming underruns (should stay 0)
  };

  // --- pure mixer math (unit-testable without a device) ---------------------

  // The user-set volume for a given bus within a mix snapshot.
  inline float busVolume(const MixSnapshot& mix, Bus bus) noexcept
  {
    switch (bus)
    {
    case Bus::Music:
      return mix.music;
    case Bus::Ambient:
      return mix.ambient;
    case Bus::Sfx:
      return mix.sfx;
    case Bus::Ui:
      return mix.ui;
    default:
      return 1.0f;
    }
  }

  // Final linear gain applied to a voice: master · bus · per-voice.
  inline float effectiveGain(const MixSnapshot& mix, Bus bus, float voiceVolume) noexcept
  {
    return mix.master * busVolume(mix, bus) * voiceVolume;
  }
} // namespace Neuron::Audio
