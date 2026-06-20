#include "pch.h"

#include "VoicePool.h"

namespace Neuron::Audio
{
  bool VoicePool::formatMatches(const WAVEFORMATEX& a, const WAVEFORMATEX& b) noexcept
  {
    return a.wFormatTag == b.wFormatTag && a.nChannels == b.nChannels &&
           a.nSamplesPerSec == b.nSamplesPerSec && a.wBitsPerSample == b.wBitsPerSample;
  }

  void VoicePool::freeSlotState(VoiceSlot& slot) noexcept
  {
    if (slot.voice != nullptr)
    {
      slot.voice->Stop(0);
      slot.voice->FlushSourceBuffers();
    }
    slot.busy = false;
    slot.looping = false;
    slot.is3d = false;
    slot.emitter = Emitter{};
    slot.baseVolume = 1.0f;
    slot.basePitch = 1.0f;
    // Generation bumped on release so prior handles go stale.
    slot.generation = (slot.generation + 1) & VOICE_GENERATION_MASK;
    if (slot.generation == 0)
      slot.generation = 1;
  }

  VoiceId VoicePool::acquire(const WAVEFORMATEX& format, IXAudio2SubmixVoice* destBus,
                             IXAudio2SourceVoice** outVoice)
  {
    if (outVoice != nullptr)
      *outVoice = nullptr;
    if (m_engine == nullptr || destBus == nullptr)
      return VoiceId{};

    const XAUDIO2_SEND_DESCRIPTOR send{0, destBus};
    const XAUDIO2_VOICE_SENDS sends{1, const_cast<XAUDIO2_SEND_DESCRIPTOR*>(&send)};

    // 1) Reuse a free slot whose existing voice matches the requested format.
    int freeAnyVoiceless = -1;
    int freeAnyMismatch = -1;
    for (size_t i = 0; i < m_slots.size(); ++i)
    {
      VoiceSlot& slot = m_slots[i];
      if (slot.busy)
        continue;
      if (slot.voice != nullptr && formatMatches(slot.format, format))
      {
        if (FAILED(slot.voice->SetOutputVoices(&sends)))
          continue;
        slot.busy = true;
        slot.bus = Bus::Sfx;
        if (outVoice != nullptr)
          *outVoice = slot.voice;
        return makeVoiceId(static_cast<uint32_t>(i), slot.generation);
      }
      if (slot.voice == nullptr && freeAnyVoiceless < 0)
        freeAnyVoiceless = static_cast<int>(i);
      else if (slot.voice != nullptr && freeAnyMismatch < 0)
        freeAnyMismatch = static_cast<int>(i);
    }

    // 2) Reuse a free slot with the wrong format by recreating its voice.
    int targetIndex = freeAnyVoiceless >= 0 ? freeAnyVoiceless : freeAnyMismatch;
    if (targetIndex < 0)
    {
      // 3) Grow the pool.
      m_slots.emplace_back();
      targetIndex = static_cast<int>(m_slots.size() - 1);
    }

    VoiceSlot& slot = m_slots[static_cast<size_t>(targetIndex)];
    if (slot.voice != nullptr)
    {
      slot.voice->DestroyVoice();
      slot.voice = nullptr;
    }

    IXAudio2SourceVoice* voice = nullptr;
    const HRESULT hr = m_engine->CreateSourceVoice(&voice, &format, 0, VOICE_MAX_FREQ_RATIO,
                                                   nullptr, &sends, nullptr);
    if (FAILED(hr) || voice == nullptr)
      return VoiceId{};

    slot.voice = voice;
    slot.format = format;
    slot.busy = true;
    slot.bus = Bus::Sfx;
    if (outVoice != nullptr)
      *outVoice = voice;
    return makeVoiceId(static_cast<uint32_t>(targetIndex), slot.generation);
  }

  VoiceSlot* VoicePool::resolve(VoiceId id) noexcept
  {
    if (!id.valid())
      return nullptr;
    const uint32_t index = voiceIndex(id);
    if (index >= m_slots.size())
      return nullptr;
    VoiceSlot& slot = m_slots[index];
    if (!slot.busy || slot.generation != voiceGeneration(id))
      return nullptr;
    return &slot;
  }

  const VoiceSlot* VoicePool::resolve(VoiceId id) const noexcept
  {
    return const_cast<VoicePool*>(this)->resolve(id);
  }

  void VoicePool::release(VoiceId id) noexcept
  {
    if (VoiceSlot* slot = resolve(id))
      freeSlotState(*slot);
  }

  uint32_t VoicePool::reclaimFinished() noexcept
  {
    uint32_t active = 0;
    for (VoiceSlot& slot : m_slots)
    {
      if (!slot.busy)
        continue;
      if (!slot.looping && slot.voice != nullptr)
      {
        XAUDIO2_VOICE_STATE state{};
        slot.voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (state.BuffersQueued == 0)
        {
          freeSlotState(slot);
          continue;
        }
      }
      ++active;
    }
    return active;
  }

  AudioStats VoicePool::stats() const noexcept
  {
    AudioStats s{};
    s.pooledVoices = static_cast<uint32_t>(m_slots.size());
    for (const VoiceSlot& slot : m_slots)
      if (slot.busy)
        ++s.activeVoices;
    return s;
  }

  void VoicePool::shutdown() noexcept
  {
    for (VoiceSlot& slot : m_slots)
    {
      if (slot.voice != nullptr)
      {
        slot.voice->Stop(0);
        slot.voice->FlushSourceBuffers();
        slot.voice->DestroyVoice();
        slot.voice = nullptr;
      }
    }
    m_slots.clear();
  }
} // namespace Neuron::Audio
