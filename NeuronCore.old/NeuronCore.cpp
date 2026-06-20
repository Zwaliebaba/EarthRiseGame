// NeuronCore.cpp
//
// Single translation unit for the NeuronCore static library (M0 Foundations).
// All subsystem headers are compiled here so the library validates them and
// produces a linkable .lib. Each subsystem is header-only at M0; implementations
// land in later milestones as .cpp files are added.

#include "pch.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <intrin.h>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

// Platform
#include "Debug.h"
#include "TimerCore.h"
#include "Allocators.h"

// ECS
#include "Ecs.h"

// World
#include "WorldPos.h"

// Serialization
#include "BitStream.h"
#include "Serde.h"

// Networking (platform-independent protocol/reliability/crypto-channel)
#include "Protocol.h"
#include "SequenceMath.h"
#include "ReplayWindow.h"
#include "Reliability.h"
#include "Fragmentation.h"
#include "PacketCodec.h"
#include "ICrypto.h"
#include "ISocket.h"
#include "SecureChannel.h"
#include "HandshakeMessages.h"
#include "Handshake.h"

// Simulation (shared sim rules + fixed-step time)
#include "Components.h"
#include "FixedStepAccumulator.h"
#include "Movement.h"
#include "Snapshot.h"

namespace Neuron
{
// Intentionally empty — all subsystem code is header-only at M0.
// Subsystem .cpp implementations are added milestone by milestone.
}
