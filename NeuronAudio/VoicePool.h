#pragma once
// VoicePool — pooled IXAudio2SourceVoice instances with generation-checked
// VoiceId handles (§11.3). Source-voice format is fixed at creation, so voices
// are reused only for a matching WAVEFORMATEX; recycling bumps the slot
// generation so a stale VoiceId from a previous play stops resolving. Per-voice
// play state (bus, emitter, looping) lives in the slot as the single source of
// truth the AudioEngine drives.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <mmreg.h>
#include <xaudio2.h>

#include <vector>

#include "AudioTypes.h"
#include "VoiceHandle.h"

namespace Neuron::Audio
{
  // Doppler clamps to 4× (SpatialMath), so source voices are created allowing a
  // frequency ratio up to this (XAUDIO2_DEFAULT_FREQ_RATIO is only 2×).
  inline constexpr float VOICE_MAX_FREQ_RATIO = 4.0f;

  struct VoiceSlot
  {
    IXAudio2SourceVoice* voice = nullptr;
    WAVEFORMATEX format{};
    uint32_t generation = 1; // starts at 1 so (index 0, gen 0) stays the null handle
    bool busy = false;
    bool looping = false;
    bool is3d = false;
    Bus bus = Bus::Sfx;
    Emitter emitter{};
    float baseVolume = 1.0f;
    float basePitch = 1.0f;
  };

  class VoicePool
  {
  public:
    void initialize(IXAudio2* engine) noexcept { m_engine = engine; }
    void shutdown() noexcept;

    // Acquire a voice for `format`, routed to `destBus`. Returns the null handle
    // if creation fails. On success *outVoice receives the source voice.
    VoiceId acquire(const WAVEFORMATEX& format, IXAudio2SubmixVoice* destBus,
                    IXAudio2SourceVoice** outVoice);

    // Resolve a (possibly stale) handle to its slot; nullptr if recycled.
    VoiceSlot* resolve(VoiceId id) noexcept;
    const VoiceSlot* resolve(VoiceId id) const noexcept;

    // Stop + free a voice (keeps the IXAudio2SourceVoice for reuse).
    void release(VoiceId id) noexcept;

    // Reclaim finished non-looping voices (called once per Update). Returns the
    // number of voices still active afterwards.
    uint32_t reclaimFinished() noexcept;

    AudioStats stats() const noexcept;
    std::vector<VoiceSlot>& slots() noexcept { return m_slots; }

  private:
    static bool formatMatches(const WAVEFORMATEX& a, const WAVEFORMATEX& b) noexcept;
    void freeSlotState(VoiceSlot& slot) noexcept;

    IXAudio2* m_engine = nullptr;
    std::vector<VoiceSlot> m_slots;
  };
} // namespace Neuron::Audio
