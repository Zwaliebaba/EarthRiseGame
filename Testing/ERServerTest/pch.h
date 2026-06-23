// pch.h: This is a precompiled header file.
//
// M5: ERServerTest now exercises the Windows persistence layer (ERServer/persist) and
// the CNG PBKDF2 path, so the PCH pulls in NeuronCore.h (which brings <Windows.h>,
// winsock, NOMINMAX, WIN32_LEAN_AND_MEAN). This matches ERServer's own pch.h so the
// Win32 TUs compiled into this test project (CngCrypto.cpp, persist/*.cpp) build
// against the same prerequisites. Windows/ODBC integration is unverified on Linux —
// validate on the Windows build agent.

#ifndef PCH_H
#define PCH_H

#include "NeuronCore.h"

#endif //PCH_H
