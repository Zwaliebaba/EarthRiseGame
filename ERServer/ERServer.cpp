// ERServer.cpp — EarthRise dedicated server entry point (M0 skeleton).
//
// Architecture (§9):
//   IOCP net threads -> decode/reliability/decrypt -> MPSC queue
//   Single-threaded 30 Hz sim (owns ECS state)
//   Persistence thread (ODBC outbox + write-behind)
//
// M0: skeleton only. Fixed-step loop with FixedStepAccumulator; no net/sim/DB yet.

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <cstdio>
#include <cstdint>

// NeuronCore
#include "platform/Debug.h"
#include "platform/TimerCore.h"
#include "sim/FixedStepAccumulator.h"

static volatile bool g_running = true;

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl)
{
    if (ctrl == CTRL_C_EVENT || ctrl == CTRL_BREAK_EVENT || ctrl == CTRL_CLOSE_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

int main()
{
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    EARTHRISE_LOG_INFO("ERServer starting (M0 skeleton — 30 Hz fixed step).\n");

    Neuron::Sim::FixedStepAccumulator acc;
    acc.Start();

    uint64_t lastLogTick = 0;

    while (g_running) {
        acc.Tick();

        while (acc.ConsumeStep()) {
            // TODO M1a: process net I/O, run sim systems, emit snapshots.
        }

        // Log heartbeat every 300 ticks (~10 seconds at 30 Hz).
        const uint64_t simTick = acc.GetSimTickCount();
        if (simTick - lastLogTick >= 300) {
            EARTHRISE_LOG_INFO("ERServer heartbeat — sim tick %llu\n", simTick);
            lastLogTick = simTick;
        }

        // Yield to avoid busy-spinning in M0. Real server wakes on IOCP events.
        Sleep(1);
    }

    EARTHRISE_LOG_INFO("ERServer shutting down.\n");
    return 0;
}
