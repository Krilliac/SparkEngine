/**
 * @file NativeTypes.h
 * @brief Native graphics and math types used by the legacy actor SDK.
 *
 * Windows uses the actual Windows SDK, DirectXMath and WRL definitions. Other
 * platforms share the engine's existing compatibility declarations; this header
 * does not introduce a second representation of any engine type.
 */

#pragma once

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <d3d11.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#else
#include <Spark/Detail/PlatformTypes.h>
#include <Spark/Detail/PlatformDirectXMathStubs.h>
#include <Spark/Detail/PlatformD3DStubs.h>
#endif

// Legacy actor signatures and protected fields use these unqualified names.
using namespace DirectX;
