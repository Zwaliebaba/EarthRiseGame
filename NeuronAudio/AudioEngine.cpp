#include "pch.h"

#include "AudioEngine.h"

namespace Neuron::Audio
{
  HRESULT AudioEngine::initialize()
  {
    if (m_initialized)
      return S_OK;

    HRESULT hr = XAudio2Create(m_engine.put(), 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr))
      return hr;

    hr = m_mixer.initialize(m_engine.get());
    if (FAILED(hr))
    {
      shutdown();
      return hr;
    }

    hr = m_spatializer.initialize(m_mixer.channelMask(), m_mixer.masterChannels());
    if (FAILED(hr))
    {
      shutdown();
      return hr;
    }

    m_pool.initialize(m_engine.get());
    m_clips.clear();
    m_clips.emplace_back(); // reserve index 0 so ClipId{0} stays the null handle
    m_initialized = true;
    return S_OK;
  }

  void AudioEngine::shutdown() noexcept
  {
    m_pool.shutdown();
    m_mixer.shutdown();
    m_clips.clear();
    m_engine = nullptr; // Release() destroys the engine after its voices
    m_initialized = false;
  }

  HRESULT AudioEngine::loadClip(std::span<const std::byte> wav, ClipId& out)
  {
    out = ClipId{};
    if (!m_initialized)
      return E_NOT_VALID_STATE;

    WavClip clip;
    const WavLoadStatus s = clip.load(wav);
    if (s != WavLoadStatus::Ok)
      return E_INVALIDARG;

    m_clips.push_back(std::move(clip));
    out = ClipId{static_cast<uint32_t>(m_clips.size() - 1)};
    return S_OK;
  }

  const WavClip* AudioEngine::resolveClip(ClipId id) const noexcept
  {
    if (id.v == 0 || id.v >= m_clips.size())
      return nullptr;
    return &m_clips[id.v];
  }

  VoiceId AudioEngine::play(ClipId clip, const PlayParams& params) noexcept
  {
    if (!m_initialized)
      return VoiceId{};
    const WavClip* wav = resolveClip(clip);
    if (wav == nullptr || wav->empty())
      return VoiceId{};

    IXAudio2SubmixVoice* busVoice = m_mixer.busVoice(params.bus);
    if (busVoice == nullptr)
      return VoiceId{};

    IXAudio2SourceVoice* voice = nullptr;
    const VoiceId id = m_pool.acquire(wav->format(), busVoice, &voice);
    if (!id.valid() || voice == nullptr)
      return VoiceId{};

    VoiceSlot* slot = m_pool.resolve(id);
    if (slot == nullptr) // should not happen immediately after acquire
      return VoiceId{};

    // 3D requires a mono clip (X3DAudio pans mono); fall back to 2D otherwise.
    const bool is3d = params.emitter != nullptr && wav->isMono();

    slot->bus = params.bus;
    slot->looping = params.loop;
    slot->is3d = is3d;
    slot->baseVolume = params.volume;
    slot->basePitch = params.pitch;
    if (is3d)
      slot->emitter = *params.emitter;

    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = static_cast<UINT32>(wav->pcm().size());
    buffer.pAudioData = reinterpret_cast<const BYTE*>(wav->pcm().data());
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    buffer.LoopCount = params.loop ? XAUDIO2_LOOP_INFINITE : 0u;

    if (FAILED(voice->SubmitSourceBuffer(&buffer)))
    {
      m_pool.release(id);
      return VoiceId{};
    }

    // Per-voice volume only; bus + master gain are applied by the graph.
    voice->SetVolume(params.volume);
    if (is3d)
      m_spatializer.apply(slot->emitter, voice, busVoice, params.pitch);
    else
      voice->SetFrequencyRatio(params.pitch);

    if (FAILED(voice->Start(0)))
    {
      m_pool.release(id);
      return VoiceId{};
    }
    return id;
  }

  void AudioEngine::stop(VoiceId id) noexcept { m_pool.release(id); }

  void AudioEngine::setVoiceVolume(VoiceId id, float volume) noexcept
  {
    if (VoiceSlot* slot = m_pool.resolve(id))
    {
      slot->baseVolume = volume;
      if (slot->voice != nullptr)
        slot->voice->SetVolume(volume);
    }
  }

  void AudioEngine::setVoicePitch(VoiceId id, float pitch) noexcept
  {
    if (VoiceSlot* slot = m_pool.resolve(id))
    {
      slot->basePitch = pitch;
      // 3D voices receive pitch (base · Doppler) from the spatializer each Update.
      if (!slot->is3d && slot->voice != nullptr)
        slot->voice->SetFrequencyRatio(pitch);
    }
  }

  bool AudioEngine::isPlaying(VoiceId id) const noexcept { return m_pool.resolve(id) != nullptr; }

  void AudioEngine::setListener(const Listener& listener) noexcept
  {
    m_listener = listener;
    m_spatializer.setListener(listener);
  }

  void AudioEngine::setMix(const MixSnapshot& mix) noexcept { m_mixer.applyMix(mix); }

  void AudioEngine::update(double /*dtSeconds*/) noexcept
  {
    if (!m_initialized)
      return;

    m_pool.reclaimFinished();

    // Re-spatialise still-active 3D voices against the current listener.
    for (VoiceSlot& slot : m_pool.slots())
    {
      if (slot.busy && slot.is3d && slot.voice != nullptr)
      {
        IXAudio2SubmixVoice* busVoice = m_mixer.busVoice(slot.bus);
        if (busVoice != nullptr)
          m_spatializer.apply(slot.emitter, slot.voice, busVoice, slot.basePitch);
      }
    }
  }

  void AudioEngine::suspend() noexcept
  {
    if (m_engine)
      m_engine->StopEngine();
  }

  void AudioEngine::resume() noexcept
  {
    if (m_engine)
      m_engine->StartEngine();
  }

  AudioStats AudioEngine::stats() const noexcept { return m_pool.stats(); }
} // namespace Neuron::Audio
