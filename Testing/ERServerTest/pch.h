// pch.h: This is a precompiled header file.
//
// M5: ERServerTest now compiles the Windows persistence layer's testable TUs directly
// (NeuronCore/CngCrypto.cpp for the PBKDF2 cross-check; ERServer/persist/OdbcConnection
// .cpp + PersistConfig.cpp for the auth-fragment / config logic). Those TUs #include
// "pch.h" first and then pull in <bcrypt.h> / <sql.h>, which need <Windows.h> already
// included — so this PCH brings in a LEAN Win32 surface (no winrt, unlike NeuronCore's
// app pch). The pure NeuronCore models the tests use (Pbkdf2.h, Outbox.h, WarmRestart.h,
// Reconnect.h) and the persist headers need no Win32 and include their own deps.
//
// Windows/ODBC integration is unverified on Linux — validate on the Windows build agent.

#ifndef PCH_H
#define PCH_H

// Match NeuronCore.h's Win32 configuration so the C++ std min/max win and the header
// surface stays small (these defines are harmless if already set).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#endif //PCH_H
