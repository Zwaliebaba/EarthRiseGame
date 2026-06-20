#pragma once
// Spatializer — thin X3DAudio wrapper (3D from day one, §11.3).
//
// The listener is the scene camera (camera-relative space, §6.4); each
// positional sound is a mono emitter. Per audio update we run X3DAudioCalculate
// for the active 3D voices and apply the resulting output matrix, Doppler ratio
// and direct low-pass to each source voice. The pure model behind the curves is
// in SpatialMath.h (unit-tested without a device).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <x3daudio.h>
#include <xaudio2.h>

#include "AudioTypes.h"

namespace Neuron::Audio
{
  class Spatializer
  {
  public:
    // `channelMask` + `outputChannels` come from the mastering voice (Mixer).
    HRESULT initialize(DWORD channelMask, uint32_t outputChannels);
    bool initialized() const noexcept { return m_initialized; }

    void setListener(const Listener& listener) noexcept;

    // Compute + apply spatialisation for one mono source routed to `destBus`.
    // `basePitch` is the voice's non-Doppler pitch; the final frequency ratio is
    // basePitch · Doppler.
    void apply(const Emitter& emitter, IXAudio2SourceVoice* source, IXAudio2SubmixVoice* destBus,
               float basePitch) noexcept;

  private:
    X3DAUDIO_HANDLE m_handle{};
    X3DAUDIO_LISTENER m_listener{};
    uint32_t m_outputChannels = 0;
    float m_matrix[8] = {}; // 1 src channel × up to 8 destination channels
    bool m_initialized = false;
  };
} // namespace Neuron::Audio
