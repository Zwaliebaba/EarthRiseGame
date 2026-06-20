// NeuronClient.cpp — single TU for the NeuronClient static library (M1b).
//
// Modules:
//   session/ — SessionImpl: encrypted reliable-UDP client session (§8.5, §10.1)
//   replica/ — ReplicaManager: snapshot decode + floating-origin projection (§8.4)
//   interp/  — InterpBuffer: snap-on-ack interpolation (§10.1)
//   control/ — IClientController / NullController

#include "pch.h"
#include <cstdint>
#include <memory>

// Core session + replica modules
#include "Session.h"
#include "SessionImpl.h"
#include "Replica.h"
#include "ReplicaManager.h"
#include "Interpolator.h"
#include "IClientController.h"

namespace Neuron::Client
{

// Stub factory kept for ERHeadless compatibility.
// EarthRise.Client constructs SessionImpl directly, injecting CngCrypto +
// WinsockSocket; tests inject FakeCrypto + LoopbackSocket.
Session* CreateSession()  { return nullptr; }
void     DestroySession(Session* s) { delete s; }

} // namespace Neuron::Client
