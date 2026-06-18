// NeuronCore.cpp
//
// Single translation unit for the NeuronCore static library (M0 Foundations).
// All subsystem headers are compiled here so the library validates them and
// produces a linkable .lib. Each subsystem is header-only at M0; implementations
// land in later milestones as .cpp files are added.

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

// Math
#include "math/MathCommon.h"
#include "math/GameMath.h"

// ECS
#include "ecs/Ecs.h"

// World
#include "world/WorldPos.h"

// Serialization
#include "serde/BitStream.h"
#include "serde/Serde.h"

// Simulation time
#include "sim/FixedStepAccumulator.h"

namespace Neuron
{
// Intentionally empty — all subsystem code is header-only at M0.
// Subsystem .cpp implementations are added milestone by milestone.
}
