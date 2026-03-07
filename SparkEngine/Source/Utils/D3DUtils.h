/**
 * @file D3DUtils.h
 * @brief Global DirectX 11 device, context, and swap chain accessors
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides global accessor functions and extern variables for the core DirectX
 * 11 objects (device, device context, swap chain) that are shared across the
 * engine. These globals are set once during GraphicsEngine initialization and
 * remain valid for the lifetime of the application.
 *
 * This module exists for backward compatibility and for subsystems that need
 * to access DirectX resources without holding a reference to the GraphicsEngine.
 * New code should prefer obtaining device pointers through the GraphicsEngine
 * or through function parameters.
 *
 * Typical usage:
 * @code
 *   // After GraphicsEngine::Initialize() has been called:
 *   ID3D11Device* device = GetD3DDevice();
 *   ID3D11DeviceContext* ctx = GetD3DContext();
 * @endcode
 *
 * @warning These globals must not be used before GraphicsEngine::Initialize()
 *          has completed, as they will be nullptr.
 * @see GraphicsEngine, GraphicsEngine::GetDevice, GraphicsEngine::GetContext
 */

#pragma once
#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <dxgi.h>
#include <d3d11.h>
#else // !SPARK_PLATFORM_WINDOWS
// Forward-declare D3D11 types as opaque structs so function signatures compile
struct IDXGISwapChain;
struct ID3D11Device;
struct ID3D11DeviceContext;
#endif // SPARK_PLATFORM_WINDOWS

#ifdef SPARK_PLATFORM_WINDOWS
/**
 * @name Global DirectX Object Pointers
 * @brief Extern pointers to the core DirectX 11 objects.
 *
 * These are set once during swap chain / device creation and should be
 * treated as read-only by all subsystems except the graphics engine initializer.
 * @{
 */
extern IDXGISwapChain* g_MainSwapChain;      ///< The primary DXGI swap chain for presentation
extern ID3D11Device* g_D3DDevice;            ///< The DirectX 11 device for resource creation
extern ID3D11DeviceContext* g_D3DContext;     ///< The immediate device context for rendering commands
/** @} */
#endif // SPARK_PLATFORM_WINDOWS

/**
 * @name Global Accessors
 * @brief Safe accessor functions for the global DirectX objects.
 * @{
 */

/** @brief Get the primary DXGI swap chain @return Pointer to the swap chain, or nullptr if not initialized */
IDXGISwapChain* GetMainSwapChain();

/** @brief Get the DirectX 11 device @return Pointer to the device, or nullptr if not initialized */
ID3D11Device* GetD3DDevice();

/** @brief Get the DirectX 11 immediate device context @return Pointer to the context, or nullptr if not initialized */
ID3D11DeviceContext* GetD3DContext();
/** @} */

/**
 * @name Global Setters
 * @brief Setter functions for manual or backward-compatible setup of DirectX globals.
 *
 * These are called internally by the GraphicsEngine during initialization.
 * External code should rarely need to call these directly.
 * @{
 */

/** @brief Set the primary DXGI swap chain @param swapChain Pointer to the swap chain */
void SetMainSwapChain(IDXGISwapChain* swapChain);

/** @brief Set the DirectX 11 device @param device Pointer to the device */
void SetD3DDevice(ID3D11Device* device);

/** @brief Set the DirectX 11 device context @param context Pointer to the device context */
void SetD3DContext(ID3D11DeviceContext* context);
/** @} */
