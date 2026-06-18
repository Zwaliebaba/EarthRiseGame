// NeuronCore.cpp
//
// Single translation unit for the NeuronCore static library at the M0 (Foundations)
// stage. The core is currently header-only (math + platform utilities provided as
// templates); this TU exists so the static library produces a .lib and so the core
// headers are compiled and validated as part of the NeuronCore build. Subsystem
// implementations (ECS, world, net, serde, sim) land in later milestones.

// Standard prerequisites pulled in first so the header-only utilities compile
// regardless of include order at their call sites.
#include <cstdint>
#include <intrin.h>
#include <type_traits>
#include <algorithm>
#include <cmath>

#include "math/MathCommon.h"
#include "math/GameMath.h"
#include "platform/Debug.h"
#include "platform/TimerCore.h"

namespace Neuron
{
  // Intentionally empty. Placeholder TU for the M0 skeleton.
}
