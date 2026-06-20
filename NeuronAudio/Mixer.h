#pragma once
// Mixer — the four submix buses under the mastering voice (§11.3).
//
// Graph: source voices → SubmixBus[bus] → mastering voice → device. Each bus is
// one IXAudio2SubmixVoice created with the master's channel count, so a 3D
// source's X3DAudio output matrix (src → bus-input channels) lines up with the
// listener channel layout. Volumes come from the user MixSnapshot (§25 client
// settings). XAudio2 voices are destroyed via DestroyVoice(), not Release(), so
// they are held as raw pointers (the IXAudio2 engine itself is COM).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <xaudio2.h>

#include "AudioTypes.h"

namespace Neuron::Audio
{
  class Mixer
  {
  public:
    // Create the mastering voice + the four submix buses. `engine` is owned by
    // the caller (AudioEngine) and must outlive the mixer.
    HRESULT initialize(IXAudio2* engine);
    void shutdown() noexcept;

    IXAudio2MasteringVoice* master() const noexcept { return m_master; }
    IXAudio2SubmixVoice* busVoice(Bus bus) const noexcept;

    // Channel layout of the mastering voice (drives X3DAudio + submix creation).
    uint32_t masterChannels() const noexcept { return m_masterChannels; }
    uint32_t masterSampleRate() const noexcept { return m_masterSampleRate; }
    DWORD channelMask() const noexcept { return m_channelMask; }

    // Push user volumes to the master + bus voices (ramped by XAudio2).
    void applyMix(const MixSnapshot& mix) noexcept;
    const MixSnapshot& mix() const noexcept { return m_mix; }

  private:
    IXAudio2MasteringVoice* m_master = nullptr;
    IXAudio2SubmixVoice* m_buses[BUS_COUNT] = {};
    uint32_t m_masterChannels = 0;
    uint32_t m_masterSampleRate = 0;
    DWORD m_channelMask = 0;
    MixSnapshot m_mix{};
  };
} // namespace Neuron::Audio
