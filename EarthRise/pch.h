#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef WINRT_LEAN_AND_MEAN
#define WINRT_LEAN_AND_MEAN
#endif

#include <windows.h>

// Direct3D 12
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>

// DirectXMath
#include <DirectXMath.h>

// C++/WinRT
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.System.h>

// STL
#include <array>
#include <cstdint>
#include <memory>
#include <queue>
#include <span>
#include <string>
#include <vector>
