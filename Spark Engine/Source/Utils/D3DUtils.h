#pragma once
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <dxgi.h>
#include <d3d11.h>
#endif // SPARK_PLATFORM_WINDOWS

// These get set once you create your swap-chain/device/context
extern IDXGISwapChain* g_MainSwapChain;
extern ID3D11Device* g_D3DDevice;
extern ID3D11DeviceContext* g_D3DContext;

// Accessors
IDXGISwapChain* GetMainSwapChain();
ID3D11Device* GetD3DDevice();
ID3D11DeviceContext* GetD3DContext();

// Setters (for backwards compatibility or manual setup)
void SetMainSwapChain(IDXGISwapChain* swapChain);
void SetD3DDevice(ID3D11Device* device);
void SetD3DContext(ID3D11DeviceContext* context);