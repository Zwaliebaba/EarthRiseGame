// NeuronCore.cpp
//
// Primary translation unit for the NeuronCore shared items project
// (NeuronCore.vcxitems). The subsystem headers are included here so they are
// compiled and validated when NeuronCore's sources build into each consumer
// (EarthRise, NeuronClient, ERServer). Subsystems start header-only; platform-
// backed implementations land in their own .cpp files (CngCrypto, WinsockSocket,
// SimComponents) as milestones progress.

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
// Intentionally empty — the header-only subsystems are validated by the includes
// above. Platform-backed subsystems compile from their own .cpp files (CngCrypto,
// WinsockSocket, SimComponents); more land milestone by milestone.
}
