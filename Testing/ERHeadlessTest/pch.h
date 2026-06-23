// pch.h — ERHeadlessTest precompiled header.
//
// ERHeadlessTest is the Windows MSTest home (§16.1) for the ERHeadless bot-harness /
// record-replay determinism substrate (§10.3). The cases here are platform-independent
// sim logic (ServerUniverse + ScriptedController + the FleetCommand wire), the same
// property the Linux testrunner DeterminismTests.cpp mirrors (§16.2) — so this PCH stays
// lean (no winrt, no <sql.h>/<bcrypt.h>). The MSTest build is validated on the Windows
// build agent; the hundreds-session contested-sector LOAD run (M4 area J) is a separate
// live ERHeadless drive, not a unit test.

#ifndef PCH_H
#define PCH_H

// Match the NeuronCore Win32 configuration so the C++ std min/max win over the Windows
// macros if any transitive header pulls <Windows.h> (harmless if already defined).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#endif // PCH_H
