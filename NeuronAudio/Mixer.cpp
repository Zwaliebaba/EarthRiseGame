#include "pch.h"

#include "Mixer.h"

namespace Neuron::Audio
{
  HRESULT Mixer::initialize(IXAudio2* engine)
  {
    if (engine == nullptr)
      return E_INVALIDARG;

    // Mastering voice with device defaults (channels / sample rate).
    HRESULT hr = engine->CreateMasteringVoice(&m_master);
    if (FAILED(hr))
      return hr;

    XAUDIO2_VOICE_DETAILS details{};
    m_master->GetVoiceDetails(&details);
    m_masterChannels = details.InputChannels;
    m_masterSampleRate = details.InputSampleRate;
    m_master->GetChannelMask(&m_channelMask);

    // One submix bus per Bus, matching the master layout so 3D output matrices
    // (src → bus input channels) align with the listener channel count.
    for (uint32_t i = 0; i < BUS_COUNT; ++i)
    {
      hr = engine->CreateSubmixVoice(&m_buses[i], m_masterChannels, m_masterSampleRate, 0, 0,
                                     nullptr, nullptr);
      if (FAILED(hr))
        return hr;
    }

    applyMix(m_mix);
    return S_OK;
  }

  void Mixer::shutdown() noexcept
  {
    for (uint32_t i = 0; i < BUS_COUNT; ++i)
    {
      if (m_buses[i] != nullptr)
      {
        m_buses[i]->DestroyVoice();
        m_buses[i] = nullptr;
      }
    }
    if (m_master != nullptr)
    {
      m_master->DestroyVoice();
      m_master = nullptr;
    }
  }

  IXAudio2SubmixVoice* Mixer::busVoice(Bus bus) const noexcept
  {
    const uint32_t i = static_cast<uint32_t>(bus);
    return i < BUS_COUNT ? m_buses[i] : nullptr;
  }

  void Mixer::applyMix(const MixSnapshot& mix) noexcept
  {
    m_mix = mix;
    if (m_master != nullptr)
      m_master->SetVolume(mix.master);
    for (uint32_t i = 0; i < BUS_COUNT; ++i)
    {
      if (m_buses[i] != nullptr)
        m_buses[i]->SetVolume(busVolume(mix, static_cast<Bus>(i)));
    }
  }
} // namespace Neuron::Audio
