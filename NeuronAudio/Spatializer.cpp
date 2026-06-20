#include "pch.h"

#include "Spatializer.h"

#include "SpatialMath.h"

namespace Neuron::Audio
{
  namespace
  {
    constexpr float PI = 3.14159265358979323846f;

    X3DAUDIO_VECTOR toX3d(const Vec3& v) noexcept { return X3DAUDIO_VECTOR{v.x, v.y, v.z}; }
  } // namespace

  HRESULT Spatializer::initialize(DWORD channelMask, uint32_t outputChannels)
  {
    if (outputChannels == 0 || outputChannels > 8)
      return E_INVALIDARG;

    const HRESULT hr = X3DAudioInitialize(channelMask, spatialmath::SPEED_OF_SOUND, m_handle);
    if (FAILED(hr))
      return hr;

    m_outputChannels = outputChannels;

    // Listener defaults: at the origin, looking down +Z, +Y up.
    m_listener = {};
    m_listener.OrientFront = X3DAUDIO_VECTOR{0.0f, 0.0f, 1.0f};
    m_listener.OrientTop = X3DAUDIO_VECTOR{0.0f, 1.0f, 0.0f};
    m_initialized = true;
    return S_OK;
  }

  void Spatializer::setListener(const Listener& listener) noexcept
  {
    m_listener.OrientFront = toX3d(listener.forward);
    m_listener.OrientTop = toX3d(listener.up);
    m_listener.Position = toX3d(listener.position);
    m_listener.Velocity = toX3d(listener.velocity);
    m_listener.pCone = nullptr;
  }

  void Spatializer::apply(const Emitter& emitter, IXAudio2SourceVoice* source,
                          IXAudio2SubmixVoice* destBus, float basePitch) noexcept
  {
    if (!m_initialized || source == nullptr || destBus == nullptr)
      return;

    // Mono emitter (X3DAudio pans mono sources). One azimuth at 0 (forward).
    float azimuth = 0.0f;
    X3DAUDIO_EMITTER em{};
    em.OrientFront = X3DAUDIO_VECTOR{0.0f, 0.0f, 1.0f};
    em.OrientTop = X3DAUDIO_VECTOR{0.0f, 1.0f, 0.0f};
    em.Position = toX3d(emitter.position);
    em.Velocity = toX3d(emitter.velocity);
    em.ChannelCount = 1;
    em.pChannelAzimuths = &azimuth;
    em.InnerRadius = emitter.minDistance;
    em.CurveDistanceScaler = emitter.maxDistance > 0.0f ? emitter.maxDistance : 1.0f;
    em.DopplerScaler = emitter.doppler ? 1.0f : 0.0f;

    X3DAUDIO_DSP_SETTINGS dsp{};
    dsp.SrcChannelCount = 1;
    dsp.DstChannelCount = m_outputChannels;
    dsp.pMatrixCoefficients = m_matrix;

    UINT32 flags = X3DAUDIO_CALCULATE_MATRIX | X3DAUDIO_CALCULATE_LPF_DIRECT;
    if (emitter.doppler)
      flags |= X3DAUDIO_CALCULATE_DOPPLER;

    X3DAudioCalculate(m_handle, &m_listener, &em, flags, &dsp);

    source->SetOutputMatrix(destBus, 1, m_outputChannels, m_matrix);

    const float doppler = emitter.doppler ? dsp.DopplerFactor : 1.0f;
    source->SetFrequencyRatio(basePitch * (doppler > 0.0f ? doppler : 1.0f));

    // Distance low-pass (muffle distant sounds). Standard XAudio2 mapping.
    XAUDIO2_FILTER_PARAMETERS filter{};
    filter.Type = LowPassFilter;
    filter.Frequency = 2.0f * std::sin(PI / 6.0f * dsp.LPFDirectCoefficient);
    filter.OneOverQ = 1.0f;
    source->SetFilterParameters(&filter);
  }
} // namespace Neuron::Audio
