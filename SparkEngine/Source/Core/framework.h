/**
 * @file framework.h
 * @brief Core framework header with essential includes and library links
 * @author Spark Engine Team
 * @date 2025
 *
 * This header provides the fundamental includes required throughout the engine,
 * including platform-specific headers, math libraries, STL containers, and
 * necessary library links. It serves as a precompiled header equivalent for
 * the engine's core dependencies.
 */

#pragma once

#include "Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS

#include "targetver.h"
//#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h> // For GET_X_LPARAM and GET_Y_LPARAM macros
#include <stdlib.h>
#include <malloc.h>
#include <memory.h>
#include <tchar.h>

// DirectX includes
#include <d3d11.h>
#include "Core/Platform.h"
#include <d3dcompiler.h>

// Link libraries
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

using namespace DirectX;

#endif // SPARK_PLATFORM_WINDOWS

// STL includes (cross-platform)
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <chrono>
#include <algorithm>
#include <queue>
#include <cstdlib>
#include <cstring>
