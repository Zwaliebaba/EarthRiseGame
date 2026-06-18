// NeuronClient.cpp
// Single TU for the NeuronClient static library (M0 skeleton).
// Modules: session (encrypted reliable-UDP + login), replica (entity mirror),
// interp (interpolation + snap-on-ack), control (IClientController -> intents).
// predict/ is deferred to post-M1 per §10.1.

#include <cstdint>
#include <memory>

#include "session/Session.h"
#include "replica/Replica.h"
#include "interp/Interpolator.h"
#include "control/IClientController.h"

namespace Neuron::Client
{
// Intentionally empty — M0 skeleton. Subsystem implementations land in M1a+.
}
