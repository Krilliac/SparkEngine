/**
 * @file GraphicsEngineWindowsInit.cpp
 * @brief Windows/D3D11 windowed initialization for GraphicsEngine
 *
 * GraphicsEngine::Initialize(hWnd) split out of GraphicsEngineWindows.cpp
 * (which keeps lifecycle: construction, device-attach initialization,
 * shutdown, and resize). Linux counterpart lives in GraphicsEngineLinux.cpp.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "GraphicsEngine.h"
#include "../Utils/Assert.h"
#include "../Utils/DebugHookManager.h"
#include "../Utils/SparkConsole.h"

#include "TextureSystem.h"
#include "MaterialSystem.h"
#include "LightingSystem.h"
#include "AssetPipeline.h"
#include "UpscalingSystem.h"
#include "VRAMBudgetMonitor.h"

#include "TemporalEffects.h"
#include "PostProcessingPipeline.h"
#include "ShadowAtlas.h"
#include "ScreenSpaceEffects.h"
#include "TerrainRenderer.h"
using Spark::Graphics::PostProcessingPipeline;
#ifdef SPARK_HYBRID_RT
#include "HybridRT/HybridRTManager.h"
#ifdef SPARK_HARDWARE_RT
#include "RHI/DXRSupport.h"
#endif
#endif
#include "GPUDrivenRenderer.h"
#include "../Physics/PhysicsSystem.h"

// Windows headers for DirectX
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include "Core/Platform.h"
#include <wrl.h>
#endif // SPARK_PLATFORM_WINDOWS

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

using Microsoft::WRL::ComPtr;

// Centralized logging macros
#include "../Utils/LogMacros.h"

// ============================================================================
// INITIALIZATION
// ============================================================================

HRESULT GraphicsEngine::Initialize(Spark::NativeWindowHandle hWnd)
// NOTE: Intentionally exceeds 50-line guideline — linear initialization sequence
{
    SPARK_DEBUG_HOOK_SYSTEM(SystemPreInit, "Graphics", 0.0);
    LOG_TO_CONSOLE_IMMEDIATE(L"GraphicsEngine::Initialize started with critical fixes.", L"INFO");
    ASSERT(hWnd != nullptr);
    if (!hWnd)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Error: hWnd is null in GraphicsEngine::Initialize", L"ERROR");
        return E_INVALIDARG;
    }

    HWND hwnd = static_cast<HWND>(hWnd);
    RECT rc;
    if (hwnd)
    {
        GetClientRect(hwnd, &rc);
    }
    else
    {
        rc.left = rc.top = rc.right = rc.bottom = 0;
        LOG_TO_CONSOLE_IMMEDIATE(L"Error: Invalid window handle in GraphicsEngine::Initialize", L"ERROR");
    }
    m_windowWidth = rc.right - rc.left;
    m_windowHeight = rc.bottom - rc.top;

    std::wstring sizeMsg = L"Window size: " + std::to_wstring(m_windowWidth) + L"x" + std::to_wstring(m_windowHeight);
    LOG_TO_CONSOLE_IMMEDIATE(sizeMsg, L"INFO");
    ASSERT_MSG(m_windowWidth > 0 && m_windowHeight > 0, "Invalid window size");

    HRESULT hr = CreateDeviceAndSwapChain(hwnd);
    ASSERT_MSG(SUCCEEDED(hr), "CreateDeviceAndSwapChain failed");
    if (FAILED(hr))
    {
        std::wstring errorMsg = L"CreateDeviceAndSwapChain failed with HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    hr = CreateRenderTargetView();
    ASSERT_MSG(SUCCEEDED(hr), "CreateRenderTargetView failed");
    if (FAILED(hr))
    {
        std::wstring errorMsg = L"CreateRenderTargetView failed with HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    hr = CreateDepthStencilView();
    ASSERT_MSG(SUCCEEDED(hr), "CreateDepthStencilView failed");
    if (FAILED(hr))
    {
        std::wstring errorMsg = L"CreateDepthStencilView failed with HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    // Rasterizer (solid/wireframe), depth-stencil, and blend states are now
    // created together in CreateRenderStates() (called below) so the same
    // shared function also equips the device-attach InitializeFromDevice()
    // path with real state objects.

    // Create GPU timing query if supported
    if (m_settings.enableGPUTiming)
    {
        D3D11_QUERY_DESC queryDesc = {};
        queryDesc.Query = D3D11_QUERY_TIMESTAMP;
        m_device->CreateQuery(&queryDesc, &m_gpuTimingQuery);
    }

    // Create advanced render targets and states
    hr = CreateAdvancedRenderTargets();
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Failed to create advanced render targets", L"WARNING");
    }

    hr = CreateRenderStates();
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Failed to create render states", L"WARNING");
    }

    SetViewport();
    ApplyGraphicsState();
    ApplyAdvancedGraphicsState();

    // Initialize advanced systems
    if (m_textureSystem)
    {
        hr = m_textureSystem->Initialize(m_device.Get(), m_context.Get());
        if (FAILED(hr))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to initialize TextureSystem", L"ERROR");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"TextureSystem initialized successfully", L"SUCCESS");
        }
    }

    if (m_materialSystem)
    {
        hr = m_materialSystem->Initialize(m_device.Get(), m_context.Get());
        if (FAILED(hr))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to initialize MaterialSystem", L"ERROR");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"MaterialSystem initialized successfully", L"SUCCESS");
        }
    }

    if (m_lightingSystem)
    {
        hr = m_lightingSystem->Initialize(m_device.Get(), m_context.Get());
        if (FAILED(hr))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to initialize LightingSystem", L"ERROR");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"LightingSystem initialized successfully", L"SUCCESS");
        }
    }

    if (m_assetPipeline)
    {
        hr = m_assetPipeline->Initialize(m_device.Get(), m_context.Get());
        if (FAILED(hr))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to initialize AssetPipeline", L"ERROR");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"AssetPipeline initialized successfully", L"SUCCESS");
        }
    }

    if (m_upscalingSystem)
    {
        if (m_upscalingSystem->Initialize(m_device.Get(), m_context.Get(), m_windowWidth, m_windowHeight))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"UpscalingSystem initialized successfully", L"SUCCESS");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to initialize UpscalingSystem", L"ERROR");
        }
    }

    if (m_vramBudgetMonitor)
    {
        hr = m_vramBudgetMonitor->Initialize(m_device.Get());
        if (FAILED(hr))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to initialize VRAMBudgetMonitor", L"ERROR");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"VRAMBudgetMonitor initialized successfully", L"SUCCESS");

            // Connect to TextureSystem for pressure-driven eviction
            if (m_textureSystem)
            {
                m_textureSystem->SetVRAMBudgetMonitor(m_vramBudgetMonitor.get());
            }
        }
    }

    if (m_physicsSystem)
    {
        hr = m_physicsSystem->Initialize();
        if (FAILED(hr))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to initialize PhysicsSystem", L"ERROR");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"PhysicsSystem initialized successfully", L"SUCCESS");
        }
    }

    // Initialize renderer integration systems
    m_pipelineStateCache.Initialize(m_device.Get());
    m_renderTargetPool.Initialize(m_device.Get());
    m_gpuSceneBuffer.Initialize(m_device.Get(), 4096);
    m_constantBufferRing.Initialize(m_device.Get());
    m_gpuDebugMarkers.Initialize(m_context.Get());
    m_gpuTimestampQuery.Initialize(m_device.Get());

    // Initialize GPU-driven renderer (Nanite-like culling pipeline)
    if (m_settings.gpuDrivenRendering)
    {
        auto& gpuRenderer = Spark::Graphics::GPUDrivenRenderer::GetInstance();
        if (gpuRenderer.Initialize(m_device.Get(), m_context.Get()))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"GPUDrivenRenderer initialized", L"SUCCESS");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"GPUDrivenRenderer init failed — falling back to CPU draw", L"WARNING");
        }
    }

    // Initialize hybrid ray tracing system (SDFGI software fallback or hardware DXR)
#ifdef SPARK_HYBRID_RT
    if (m_rhiBridge)
    {
        m_hybridRT = std::make_unique<Spark::Graphics::HybridRTManager>();
        if (!m_hybridRT->Initialize(m_rhiBridge->GetDevice(), m_windowWidth, m_windowHeight))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"HybridRT: Falling back to screen-space only", L"WARNING");
            m_hybridRT.reset();
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"HybridRT initialized successfully", L"SUCCESS");
#ifdef SPARK_HARDWARE_RT
            if (m_hybridRT->GetActiveBackend() == Spark::RHI::RayTracingBackend::HardwareDXR)
            {
                auto& dxr = Spark::Graphics::DXRManager::GetInstance();
                if (!dxr.IsAvailable())
                {
                    LOG_TO_CONSOLE_IMMEDIATE(L"HybridRT: DXR hardware backend selected", L"INFO");
                }
            }
#endif
        }
    }
#endif

    // Initialize post-processing pipeline with D3D11 device
    if (m_postProcessing)
    {
        m_postProcessing->SetDevice(m_device.Get(), m_context.Get());
        if (m_postProcessing->Initialize(m_windowWidth, m_windowHeight))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"PostProcessingPipeline initialized successfully", L"SUCCESS");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"PostProcessingPipeline GPU init failed (CPU-only mode)", L"WARNING");
        }
    }

    // Initialize temporal effects (TAA, motion blur) with D3D11 device
    if (m_temporalEffects)
    {
        m_temporalEffects->SetDevice(m_device.Get(), m_context.Get());
        m_temporalEffects->Initialize(m_windowWidth, m_windowHeight);
        if (m_temporalEffects->InitializeGPU())
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"TemporalEffects initialized successfully", L"SUCCESS");
        }
        else
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"TemporalEffects GPU init failed (CPU-only mode)", L"WARNING");
        }
    }

    // Initialize shadow atlas for shadow map management
    if (m_shadowAtlas)
    {
        uint32_t shadowAtlasSize = m_settings.shadowMapSize * 2;
        if (m_shadowAtlas->Initialize(shadowAtlasSize))
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"ShadowAtlas initialized successfully", L"SUCCESS");
        }
    }

    // Initialize screen-space effects (SSAO, SSR, contact shadows)
    if (m_screenSpaceEffects)
    {
        if (m_screenSpaceEffects->Initialize(m_windowWidth, m_windowHeight))
        {
            m_screenSpaceEffects->SetSSAOEnabled(m_settings.ssao);
            LOG_TO_CONSOLE_IMMEDIATE(L"ScreenSpaceEffects initialized successfully", L"SUCCESS");
        }
    }

    // Initialize terrain renderer
    if (m_terrainRenderer)
    {
#ifdef SPARK_PLATFORM_WINDOWS
        if (m_terrainRenderer->Initialize(m_device.Get()))
#else
        if (m_terrainRenderer->Initialize())
#endif
        {
            LOG_TO_CONSOLE_IMMEDIATE(L"TerrainRenderer initialized", L"SUCCESS");
        }
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"GraphicsEngine initialization complete - rendering ready.", L"SUCCESS");

    // Initialize basic shaders for rendering
    HRESULT shaderResult = InitializeBasicShaders();
    if (FAILED(shaderResult))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Failed to initialize basic shaders", L"WARNING");
    }
    else
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Basic shaders initialized successfully", L"SUCCESS");
    }

    // Phase Q: activate the image denoiser. The default backend is
    // SoftwareDenoiser (joint-bilateral filter), which works on every
    // platform with no external SDK. A future pipeline can replace
    // m_denoiser with an OIDN / OptiX instance before the first
    // Execute() call. The initial settings have `enabled = false` so
    // no CPU work happens until a real RT pass toggles it on.
    m_denoiser = std::make_unique<Spark::Graphics::SoftwareDenoiser>();
    Spark::Graphics::DenoiserSettings denoiserSettings;
    denoiserSettings.backend = Spark::Graphics::DenoiserBackend::Software;
    denoiserSettings.quality = Spark::Graphics::DenoiserQuality::Balanced;
    m_denoiser->Initialize(denoiserSettings);

    // Phase S: activate the procedural noise graph. Default output
    // is a SimplexNode so the accessor is useful immediately; terrain
    // / foliage / decoration systems can add more nodes via
    // `GetProceduralNoise()->AddNode(...)` without rebuilding the
    // engine.
    m_proceduralNoise = std::make_unique<Spark::Graphics::NoiseGraph>();
    {
        auto defaultNode = std::make_unique<Spark::Graphics::SimplexNode>();
        auto* nodePtr = m_proceduralNoise->AddNode(std::move(defaultNode));
        m_proceduralNoise->SetOutputNode(nodePtr);
    }

    // Phase T: activate the voxel cone traced GI system with a
    // small 32³ default grid (~130 KB) and `enabled = false`. A
    // future GI render pass that wants full resolution can
    // re-initialise via `GetVCTSystem()->Initialize({...})` with a
    // 128³ grid. Keeping the default small avoids wasting ~9 MB
    // per GraphicsEngine instance for a feature that is opt-in.
    m_vctSystem = std::make_unique<Spark::Graphics::VCTSystem>();
    {
        Spark::Graphics::VCTSettings vctSettings;
        vctSettings.enabled = false;
        vctSettings.voxelResolution = 32;
        vctSettings.worldExtent = 50.0f;
        m_vctSystem->Initialize(vctSettings);
    }

    SPARK_DEBUG_HOOK_SYSTEM(SystemPostInit, "Graphics", 0.0);
    return S_OK;
}

#endif // SPARK_PLATFORM_WINDOWS
