// NeuronClient.cpp — single TU for the NeuronClient static library (M1b).
//
// Files are flat in the project, grouped by Visual Studio Filters:
//   Session — Session/SessionImpl: encrypted reliable-UDP client session (§8.5, §10.1)
//   Replica — Replica/ReplicaManager: snapshot decode + floating-origin projection (§8.4)
//   Interpolator — snap-on-ack interpolation buffer (§10.1)
//   Control — IClientController / NullController

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
// EarthRise.Client constructs SessionImpl directly, injecting CngCrypto + the WinRT
// DatagramSocketAdapter (§8.1); ERHeadless injects WinsockSocket; tests inject
// FakeCrypto + LoopbackSocket.
Session* CreateSession()  { return nullptr; }
void     DestroySession(Session* s) { delete s; }

} // namespace Neuron::Client
