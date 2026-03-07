#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "D3DUtils.h"
#include "../Graphics/GraphicsEngine.h"
#include <iostream>

// Define the globals
IDXGISwapChain* g_MainSwapChain = nullptr;
ID3D11Device* g_D3DDevice = nullptr;
ID3D11DeviceContext* g_D3DContext = nullptr;

// External reference to the global graphics engine
extern std::unique_ptr<GraphicsEngine> g_graphics;

// Implement the accessors
IDXGISwapChain* GetMainSwapChain() { 
    if (g_graphics) {
        return g_graphics->GetSwapChain();
    }
    return g_MainSwapChain; 
}

ID3D11Device* GetD3DDevice() { 
    if (g_graphics) {
        return g_graphics->GetDevice();
    }
    return g_D3DDevice; 
}

ID3D11DeviceContext* GetD3DContext() { 
    if (g_graphics) {
        return g_graphics->GetContext();
    }
    return g_D3DContext; 
}

// Functions to set the globals (for backwards compatibility or manual setup)
void SetMainSwapChain(IDXGISwapChain* swapChain) {
    g_MainSwapChain = swapChain;
}

void SetD3DDevice(ID3D11Device* device) {
    g_D3DDevice = device;
}

void SetD3DContext(ID3D11DeviceContext* context) {
    g_D3DContext = context;
}
#endif // SPARK_PLATFORM_WINDOWS
