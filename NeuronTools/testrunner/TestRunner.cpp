// TestRunner.cpp — M0 unit test entry point.
//
// Runs all NeuronCore M0 tests:
//   Math        — DirectXMath wrappers
//   World       — WorldPos / SectorId / sector math / FloatingOrigin
//   ECS         — entity handles, component add/get/remove, ForEach
//   Serde       — BitWriter/BitReader, WriteBuffer/ReadBuffer, version check
//   FixedStep   — FixedStepAccumulator, catch-up guard, alpha, tick count
//
// Exit code 0 = all pass; non-zero = at least one failure.

// Pull in subsystem headers first (GameMath.h needs DirectXMath).
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

// Test suites (each #includes the relevant NeuronCore headers).
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

// Test runner framework.
#include "TestRunner.h"

int main()
{
    return Neuron::Test::RunAll();
}
