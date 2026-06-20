#pragma once
// Generation-checked VoiceId packing (device-free, header-only).
//
// A VoiceId packs a slot index with a generation counter, exactly like the
// NeuronCore ECS 32-bit handles (§7.2): when a pooled source voice is recycled
// its slot's generation is bumped, so a stale VoiceId from a previous play no
// longer resolves. Index 0 / generation 0 is reserved as the null handle.

#include <cstdint>

#include "AudioTypes.h"

namespace Neuron::Audio
{
  inline constexpr uint32_t VOICE_INDEX_BITS = 20;                       // up to ~1M slots
  inline constexpr uint32_t VOICE_INDEX_MASK = (1u << VOICE_INDEX_BITS) - 1;
  inline constexpr uint32_t VOICE_GENERATION_MASK = (1u << (32 - VOICE_INDEX_BITS)) - 1;

  inline constexpr VoiceId makeVoiceId(uint32_t index, uint32_t generation) noexcept
  {
    return VoiceId{(index & VOICE_INDEX_MASK) | ((generation & VOICE_GENERATION_MASK) << VOICE_INDEX_BITS)};
  }

  inline constexpr uint32_t voiceIndex(VoiceId id) noexcept { return id.v & VOICE_INDEX_MASK; }

  inline constexpr uint32_t voiceGeneration(VoiceId id) noexcept
  {
    return (id.v >> VOICE_INDEX_BITS) & VOICE_GENERATION_MASK;
  }
} // namespace Neuron::Audio
