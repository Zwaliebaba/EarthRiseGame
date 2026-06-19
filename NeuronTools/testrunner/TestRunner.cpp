// TestRunner.cpp — unit test entry point.
//
// Runs all NeuronCore M0 + M1a + M1b (client-interp) tests:
//   Math         — DirectXMath wrappers
//   World        — WorldPos / SectorId / sector math / FloatingOrigin
//   ECS          — entity handles, component add/get/remove, ForEach
//   Serde        — BitWriter/BitReader, WriteBuffer/ReadBuffer, version check
//   FixedStep    — FixedStepAccumulator, catch-up guard, alpha, tick count
//   Net          — reliability, fragmentation, dup/loss/reorder
//   Crypto       — AES-GCM AEAD nonce/replay; handshake vectors
//   Loopback     — ClientConnection ↔ server over in-process loopback
//   Sim          — SimWorld fixed-step, entity movement, sector crossing
//   Handshake    — full §8.5 sequence, MITM / replay / dup / reorder cases
//   ClientInterp — InterpBuffer snap-on-ack, lerp, Advance (M1b)
//
// Exit code 0 = all pass; non-zero = at least one failure.

#include <cstdint>
#include <cstdlib>
#include <stdexcept>

// Test suites (each #includes the relevant NeuronCore/NeuronClient headers).
#include "Tests_Math.h"
#include "Tests_World.h"
#include "Tests_ECS.h"
#include "Tests_Serde.h"
#include "Tests_FixedStep.h"
#include "Tests_Net.h"
#include "Tests_Crypto.h"
#include "Tests_Loopback.h"
#include "Tests_Sim.h"
#include "Tests_Handshake.h"
#include "Tests_ClientInterp.h"

// Test runner framework.
#include "TestRunner.h"

int main()
{
    return Neuron::Test::RunAll();
}
