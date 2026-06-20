#pragma once
// AudioEngine — the NeuronAudio front door (§11.3; neuronaudio-api.md §6).
//
// Owns the IXAudio2 engine, the four-bus Mixer, the X3DAudio Spatializer and the
// VoicePool, plus an in-memory clip registry. The UWP client feeds the listener
// from the same camera it renders with and calls Update() once per frame on the
// game/render thread. ERHeadless does NOT link this library.
//
// Scope (M2): in-memory PCM-16 clips, looped for ambient/music. Buffer-queue
// streaming (WavStream) and the data-driven CueCatalog/AudioEventRouter are the
// next NeuronAudio increment — see README/M2 plan area E.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <winrt/base.h>
#include <xaudio2.h>

#include <cstddef>
#include <span>
#include <vector>

#include "AudioTypes.h"
#include "Mixer.h"
#include "Spatializer.h"
#include "VoicePool.h"
#include "WavReader.h"

namespace Neuron::Audio
{
  class AudioEngine
  {
  public:
    // Create IXAudio2, the mastering voice, the four buses and X3DAudio. The
    // caller's thread must already have COM initialised (the UWP framework does
    // this); NeuronAudio does not CoInitialize on the app's behalf.
    HRESULT initialize();
    void shutdown() noexcept;
    bool initialized() const noexcept { return m_initialized; }

    // ---- asset registry ----
    HRESULT loadClip(std::span<const std::byte> wav, ClipId& out);

    // ---- playback ----
    VoiceId play(ClipId clip, const PlayParams& params) noexcept; // {} if pool exhausted
    void stop(VoiceId id) noexcept;
    void setVoiceVolume(VoiceId id, float volume) noexcept;
    void setVoicePitch(VoiceId id, float pitch) noexcept;
    bool isPlaying(VoiceId id) const noexcept;

    // ---- mixing & spatial (once per frame) ----
    void setListener(const Listener& listener) noexcept;
    void setMix(const MixSnapshot& mix) noexcept;
    void update(double dtSeconds) noexcept;

    // ---- UWP lifecycle (§11.3) ----
    void suspend() noexcept; // StopEngine
    void resume() noexcept;  // StartEngine

    AudioStats stats() const noexcept;

  private:
    const WavClip* resolveClip(ClipId id) const noexcept;

    winrt::com_ptr<IXAudio2> m_engine;
    Mixer m_mixer;
    Spatializer m_spatializer;
    VoicePool m_pool;
    std::vector<WavClip> m_clips; // index 0 reserved as the null clip
    Listener m_listener{};
    bool m_initialized = false;
  };
} // namespace Neuron::Audio
