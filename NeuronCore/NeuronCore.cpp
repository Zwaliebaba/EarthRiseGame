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
#include "platform/Debug.h"
#include "platform/TimerCore.h"
#include "platform/Allocators.h"

// ECS
#include "ecs/Ecs.h"

// World
#include "world/WorldPos.h"

// Serialization
#include "serde/BitStream.h"
#include "serde/Serde.h"

// Networking (platform-independent protocol/reliability/crypto-channel)
#include "net/Protocol.h"
#include "net/SequenceMath.h"
#include "net/ReplayWindow.h"
#include "net/Reliability.h"
#include "net/Fragmentation.h"
#include "net/PacketCodec.h"
#include "net/ICrypto.h"
#include "net/ISocket.h"
#include "net/SecureChannel.h"
#include "net/HandshakeMessages.h"
#include "net/Handshake.h"

// Simulation (shared sim rules + fixed-step time)
#include "sim/Components.h"
#include "sim/FixedStepAccumulator.h"
#include "sim/Movement.h"
#include "sim/Snapshot.h"

namespace Neuron
{
// Intentionally empty — all subsystem code is header-only at M0.
// Subsystem .cpp implementations are added milestone by milestone.
}
