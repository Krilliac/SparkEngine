#include "Core/Platform.h"
#include "Validate.h"

#ifdef SPARK_PLATFORM_WINDOWS

#include "D3DUtils.h"
#include "../Graphics/GraphicsEngine.h"
#include <iostream>

// Define the globals
IDXGISwapChain* g_MainSwapChain = nullptr;
ID3D11Device* g_D3DDevice = nullptr;
ID3D11DeviceContext* g_D3DContext = nullptr;

// Access graphics engine via EngineContext (preferred) or legacy global
#include "../Core/EngineContext.h"

// Implement the accessors — use EngineContext to access GraphicsEngine
static GraphicsEngine* GetGraphicsFromContext()
{
    auto* ctx = EngineContext::Get();
    SPARK_WARN_IF_NULL(Spark::LogCategory::Graphics, ctx);
    return ctx ? ctx->GetGraphics() : nullptr;
}

IDXGISwapChain* GetMainSwapChain()
{
    if (auto* gfx = GetGraphicsFromContext())
        return gfx->GetSwapChain();
    return g_MainSwapChain;
}

ID3D11Device* GetD3DDevice()
{
    if (auto* gfx = GetGraphicsFromContext())
        return gfx->GetDevice();
    return g_D3DDevice;
}

ID3D11DeviceContext* GetD3DContext()
{
    if (auto* gfx = GetGraphicsFromContext())
        return gfx->GetContext();
    return g_D3DContext;
}

// Functions to set the globals (for backwards compatibility or manual setup)
void SetMainSwapChain(IDXGISwapChain* swapChain)
{
    g_MainSwapChain = swapChain;
}

void SetD3DDevice(ID3D11Device* device)
{
    g_D3DDevice = device;
}

void SetD3DContext(ID3D11DeviceContext* context)
{
    g_D3DContext = context;
}

#else // !SPARK_PLATFORM_WINDOWS

#include "D3DUtils.h"

// No-op stubs for non-Windows platforms
IDXGISwapChain* GetMainSwapChain()
{
    return nullptr;
}
ID3D11Device* GetD3DDevice()
{
    return nullptr;
}
ID3D11DeviceContext* GetD3DContext()
{
    return nullptr;
}
void SetMainSwapChain(IDXGISwapChain*) {}
void SetD3DDevice(ID3D11Device*) {}
void SetD3DContext(ID3D11DeviceContext*) {}

#endif // SPARK_PLATFORM_WINDOWS
