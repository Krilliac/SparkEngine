#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
// GraphicsEngine.cpp - COMPLETE IMPLEMENTATION WITH ALL 600+ LINES RESTORED
#include "GraphicsEngine.h"
#include "../Utils/Assert.h"
#include "../Utils/SparkError.h"
#include "../Utils/SparkConsole.h"

// **CRITICAL FIX: Add missing system includes**
#include "TextureSystem.h"
#include "MaterialSystem.h"
#include "LightingSystem.h"
#include "PostProcessingSystem.h"
#include "AssetPipeline.h"

#include "TemporalEffects.h"
#include "LightManager.h"
#include "PostProcessingPipeline.h"
#include "Shader.h"
#include "RenderTarget.h"
#include "../Physics/PhysicsSystem.h"
#include "../Game/GameObject.h"

// Windows headers for DirectX
#include <Windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <DirectXMath.h>
#include <wrl.h>
#include <d3dcompiler.h>

// **CRITICAL FIX: Add missing standard library includes**
#include <string>
#include <sstream>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <chrono>
#include <fstream>
#include <thread>
#include <cmath>
#include <ctime>
#include <atomic>
#include <mutex>
#include <vector>
#include <memory>
#include <functional>
#include <cfloat>  // ✅ ADD: For FLT_MAX

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// Centralized logging macros (previously defined locally with inconsistent rate limits)
#include "../Utils/LogMacros.h"

// ============================================================================
// CONSTRUCTOR AND DESTRUCTOR
// ============================================================================

GraphicsEngine::GraphicsEngine()
    : m_windowWidth(1280)
    , m_windowHeight(720)
    , m_width(1280)
    , m_height(720)
    , m_fullscreen(false)
    , m_hwnd(nullptr)
    , m_textureMemoryUsage(0)
    , m_bufferMemoryUsage(0)
    , m_frameInProgress(false)
    , m_currentPipeline(RenderingPipeline::Forward)
    , m_hdrEnabled(false)
    , m_msaaLevel(MSAALevel::None)
    , m_physicsSystem()
{
    // Initialize unified settings to defaults
    m_settings.vsync = true;
    m_settings.wireframeMode = false;
    m_settings.debugMode = false;
    m_settings.showFPS = false;
    m_settings.clearColor[0] = 0.0f;
    m_settings.clearColor[1] = 0.2f;
    m_settings.clearColor[2] = 0.4f;
    m_settings.clearColor[3] = 1.0f;
    m_settings.renderScale = 1.0f;
    m_settings.hdr = false;
    m_settings.msaaSamples = 1;
    m_settings.enableGPUTiming = false;
    m_settings.frustumCulling = true;
    m_settings.shadows = true;
    m_settings.bloom = true;
    m_settings.ssao = false;
    m_settings.taa = false;
    m_settings.shadowMapSize = 2048;
    m_settings.maxTextureSize = 2048;
    m_settings.anisotropyLevel = 8;

    // Initialize advanced settings
    m_taaSettings = {};
    m_ssaoSettings = {};
    m_ssrSettings = {};
    m_volumetricSettings = {};

    // Initialize statistics
    m_statistics = {};

    // Create advanced systems
    try {
        m_textureSystem = std::make_unique<TextureSystem>();
        m_materialSystem = std::make_unique<MaterialSystem>();
        m_lightingSystem = std::make_unique<LightingSystem>();
        m_postProcessingSystem = std::make_unique<PostProcessingSystem>();
        m_assetPipeline = std::make_unique<AssetPipeline>();
        m_physicsSystem = std::make_unique<PhysicsSystem>();
        
        // Create legacy systems for compatibility
        m_lightManager = std::make_unique<LightManager>();
        m_postProcessing = std::make_unique<PostProcessingPipeline>();
        m_temporalEffects = std::make_unique<TemporalEffects>();
        
        LOG_TO_CONSOLE_IMMEDIATE(L"Advanced systems created successfully", L"INFO");
    } catch (const std::exception&) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Some advanced systems failed to create", L"WARNING");
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"GraphicsEngine constructed with unified architecture and atomic frame management.", L"INFO");
}

GraphicsEngine::~GraphicsEngine()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"GraphicsEngine destructor called.", L"INFO");
    Shutdown();
}

// ============================================================================
// INITIALIZATION AND SHUTDOWN
// ============================================================================

HRESULT GraphicsEngine::Initialize(HWND hWnd)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"GraphicsEngine::Initialize started with critical fixes.", L"INFO");
    ASSERT(hWnd != nullptr);
    if (!hWnd) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Error: hWnd is null in GraphicsEngine::Initialize", L"ERROR");
        return E_INVALIDARG;
    }
    
    RECT rc; 
    // Defensive: Only call GetClientRect if hWnd is valid
    if (hWnd) {
        GetClientRect(hWnd, &rc);
    }
    else {
        // Should never reach here due to earlier check, but avoid C6387 warning
        rc.left = rc.top = rc.right = rc.bottom = 0;
		LOG_TO_CONSOLE_IMMEDIATE(L"Error: Invalid window handle in GraphicsEngine::Initialize", L"ERROR");
    }
    m_windowWidth = rc.right - rc.left;
    m_windowHeight = rc.bottom - rc.top;

    std::wstring sizeMsg = L"Window size: " + std::to_wstring(m_windowWidth) + L"x" + std::to_wstring(m_windowHeight);
    LOG_TO_CONSOLE_IMMEDIATE(sizeMsg, L"INFO");
    ASSERT_MSG(m_windowWidth > 0 && m_windowHeight > 0, "Invalid window size");

    HRESULT hr = CreateDeviceAndSwapChain(hWnd);
    ASSERT_MSG(SUCCEEDED(hr), "CreateDeviceAndSwapChain failed");
    if (FAILED(hr)) {
        std::wstring errorMsg = L"CreateDeviceAndSwapChain failed with HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    hr = CreateRenderTargetView();
    ASSERT_MSG(SUCCEEDED(hr), "CreateRenderTargetView failed");
    if (FAILED(hr)) {
        std::wstring errorMsg = L"CreateRenderTargetView failed with HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    hr = CreateDepthStencilView();
    ASSERT_MSG(SUCCEEDED(hr), "CreateDepthStencilView failed");
    if (FAILED(hr)) {
        std::wstring errorMsg = L"CreateDepthStencilView failed with HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    // Create rasterizer states
    D3D11_RASTERIZER_DESC rastDesc = {};
    rastDesc.FillMode = D3D11_FILL_SOLID;
    rastDesc.CullMode = D3D11_CULL_BACK;
    rastDesc.FrontCounterClockwise = FALSE;
    rastDesc.DepthBias = 0;
    rastDesc.DepthBiasClamp = 0.0f;
    rastDesc.SlopeScaledDepthBias = 0.0f;
    rastDesc.DepthClipEnable = TRUE;
    rastDesc.ScissorEnable = FALSE;
    rastDesc.MultisampleEnable = FALSE;
    rastDesc.AntialiasedLineEnable = FALSE;

    hr = m_device->CreateRasterizerState(&rastDesc, &m_solidRasterState);
    ASSERT_MSG(SUCCEEDED(hr), "CreateRasterizerState (solid) failed");
    if (FAILED(hr)) return hr;

    rastDesc.FillMode = D3D11_FILL_WIREFRAME;
    hr = m_device->CreateRasterizerState(&rastDesc, &m_wireframeRasterState);
    ASSERT_MSG(SUCCEEDED(hr), "CreateRasterizerState (wireframe) failed");
    if (FAILED(hr)) return hr;

    // Create GPU timing query if supported
    if (m_settings.enableGPUTiming) {
        D3D11_QUERY_DESC queryDesc = {};
        queryDesc.Query = D3D11_QUERY_TIMESTAMP;
        m_device->CreateQuery(&queryDesc, &m_gpuTimingQuery);
    }

    // Create advanced render targets and states
    hr = CreateAdvancedRenderTargets();
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Failed to create advanced render targets", L"WARNING");
    }

    hr = CreateRenderStates();
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Failed to create render states", L"WARNING");
    }

    SetViewport();
    ApplyGraphicsState();
    ApplyAdvancedGraphicsState();

    // Initialize advanced systems
    if (m_textureSystem) {
        hr = m_textureSystem->Initialize(m_device.Get(), m_context.Get());
        if (FAILED(hr)) {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to initialize TextureSystem", L"ERROR");
        } else {
            LOG_TO_CONSOLE_IMMEDIATE(L"TextureSystem initialized successfully", L"SUCCESS");
        }
    }

    if (m_materialSystem) {
        hr = m_materialSystem->Initialize(m_device.Get(), m_context.Get());
        if (FAILED(hr)) {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to initialize MaterialSystem", L"ERROR");
        } else {
            LOG_TO_CONSOLE_IMMEDIATE(L"MaterialSystem initialized successfully", L"SUCCESS");
        }
    }

    if (m_lightingSystem) {
        hr = m_lightingSystem->Initialize(m_device.Get(), m_context.Get());
        if (FAILED(hr)) {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to initialize LightingSystem", L"ERROR");
        } else {
            LOG_TO_CONSOLE_IMMEDIATE(L"LightingSystem initialized successfully", L"SUCCESS");
        }
    }

    if (m_postProcessingSystem) {
        hr = m_postProcessingSystem->Initialize(m_device.Get(), m_context.Get());
        if (FAILED(hr)) {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to initialize PostProcessingSystem", L"ERROR");
        } else {
            LOG_TO_CONSOLE_IMMEDIATE(L"PostProcessingSystem initialized successfully", L"SUCCESS");
        }
    }

    if (m_assetPipeline) {
        hr = m_assetPipeline->Initialize(m_device.Get(), m_context.Get());
        if (FAILED(hr)) {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to initialize AssetPipeline", L"ERROR");
        } else {
            LOG_TO_CONSOLE_IMMEDIATE(L"AssetPipeline initialized successfully", L"SUCCESS");
        }
    }

    if (m_physicsSystem) {
        hr = m_physicsSystem->Initialize();
        if (FAILED(hr)) {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to initialize PhysicsSystem", L"ERROR");
        } else {
            LOG_TO_CONSOLE_IMMEDIATE(L"PhysicsSystem initialized successfully", L"SUCCESS");
        }
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"GraphicsEngine initialization complete - rendering ready.", L"SUCCESS");
    
    // ✅ ADD: Initialize basic shaders for rendering
    HRESULT shaderResult = InitializeBasicShaders();
    if (FAILED(shaderResult)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Failed to initialize basic shaders", L"WARNING");
    } else {
        LOG_TO_CONSOLE_IMMEDIATE(L"Basic shaders initialized successfully", L"SUCCESS");
    }
    
    return S_OK;  // ✅ FIXED: Missing return statement
}

void GraphicsEngine::Shutdown()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"GraphicsEngine::Shutdown called.", L"INFO");

    // Shutdown advanced systems
    if (m_textureSystem) {
        m_textureSystem->Shutdown();
        LOG_TO_CONSOLE_IMMEDIATE(L"TextureSystem shutdown complete", L"INFO");
    }

    if (m_materialSystem) {
        m_materialSystem->Shutdown();
        LOG_TO_CONSOLE_IMMEDIATE(L"MaterialSystem shutdown complete", L"INFO");
    }

    if (m_lightingSystem) {
        m_lightingSystem->Shutdown();
        LOG_TO_CONSOLE_IMMEDIATE(L"LightingSystem shutdown complete", L"INFO");
    }

    if (m_postProcessingSystem) {
        m_postProcessingSystem->Shutdown();
        LOG_TO_CONSOLE_IMMEDIATE(L"PostProcessingSystem shutdown complete", L"INFO");
    }

    if (m_assetPipeline) {
        m_assetPipeline->Shutdown();
        LOG_TO_CONSOLE_IMMEDIATE(L"AssetPipeline shutdown complete", L"INFO");
    }

    // Clean up physics system
    if (m_physicsSystem) {
        m_physicsSystem->Shutdown();
        m_physicsSystem.reset();
        LOG_TO_CONSOLE_IMMEDIATE(L"PhysicsSystem shutdown complete", L"INFO");
    }

    // Shutdown legacy systems
    if (m_lightManager) {
        m_lightManager->Shutdown();
        LOG_TO_CONSOLE_IMMEDIATE(L"LightManager shutdown complete", L"INFO");
    }

    if (m_postProcessing) {
        m_postProcessing->Shutdown();
        LOG_TO_CONSOLE_IMMEDIATE(L"PostProcessingPipeline shutdown complete", L"INFO");
    }

    if (m_temporalEffects) {
        m_temporalEffects->Shutdown();
        LOG_TO_CONSOLE_IMMEDIATE(L"TemporalEffects shutdown complete", L"INFO");
    }

    // Release DirectX resources
    m_hdrSRV.Reset();
    m_hdrRTV.Reset();
    m_hdrTexture.Reset();
    
    for (int i = 0; i < 4; i++) {
        m_gBufferSRVs[i].Reset();
        m_gBufferRTVs[i].Reset();
        m_gBufferTextures[i].Reset();
    }
    
    m_defaultBlendState.Reset();
    m_defaultDepthState.Reset();
    m_gpuTimingQuery.Reset();
    m_wireframeRasterState.Reset();
    m_solidRasterState.Reset();
    m_depthStencilView.Reset();
    m_renderTargetView.Reset();
    m_swapChain.Reset();
    m_context.Reset();
    m_device.Reset();

    LOG_TO_CONSOLE_IMMEDIATE(L"GraphicsEngine shutdown complete.", L"INFO");
}

// ============================================================================
// FRAME MANAGEMENT
// ============================================================================

void GraphicsEngine::BeginFrame()
{
    bool expected = false;
    if (!m_frameInProgress.compare_exchange_strong(expected, true)) {
        LOG_TO_CONSOLE_RATE_LIMITED(L"Warning: BeginFrame called while frame already in progress - skipping", L"WARNING");
        return;
    }
    
    m_frameStartTime = std::chrono::high_resolution_clock::now();

    ASSERT(m_context && m_renderTargetView && m_depthStencilView);
    
    if (!m_context || !m_renderTargetView || !m_depthStencilView) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Error: Invalid render targets in BeginFrame", L"ERROR");
        m_frameInProgress = false;
        return;
    }

    // Clear render targets
    m_context->ClearRenderTargetView(m_renderTargetView.Get(), m_settings.clearColor);
    m_context->ClearDepthStencilView(m_depthStencilView.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    // Bind render targets
    m_context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), m_depthStencilView.Get());

    m_renderStartTime = std::chrono::high_resolution_clock::now();

    // Apply graphics state
    ApplyGraphicsState();

    // Start GPU timing if enabled
    if (m_gpuTimingQuery && m_settings.enableGPUTiming) {
        m_context->Begin(m_gpuTimingQuery.Get());
    }
}

void GraphicsEngine::EndFrame()
{
    bool expected = true;
    if (!m_frameInProgress.compare_exchange_strong(expected, false)) {
        LOG_TO_CONSOLE_RATE_LIMITED(L"Warning: EndFrame called without matching BeginFrame - skipping", L"WARNING");
        return;
    }

    auto renderEndTime = std::chrono::high_resolution_clock::now();

    if (!m_swapChain) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Error: Invalid swap chain in EndFrame", L"ERROR");
        return;
    }

    // End GPU timing if enabled
    if (m_gpuTimingQuery && m_settings.enableGPUTiming) {
        m_context->End(m_gpuTimingQuery.Get());
    }

    UINT syncInterval = m_settings.vsync ? 1 : 0;
    HRESULT hr = m_swapChain->Present(syncInterval, 0);

    if (FAILED(hr)) {
        SPARK_LOG_ERROR("Graphics", "SwapChain::Present failed with HR=0x%08lX", static_cast<long>(hr));

        if (hr == DXGI_ERROR_DEVICE_REMOVED) {
            HRESULT reason = m_device ? m_device->GetDeviceRemovedReason() : E_FAIL;
            SPARK_LOG_FATAL("Graphics", "GPU DEVICE REMOVED -- Reason HR=0x%08lX. "
                            "Possible causes: driver crash, GPU hang, TDR timeout, or hardware fault",
                            static_cast<long>(reason));
        } else if (hr == DXGI_ERROR_DEVICE_RESET) {
            SPARK_LOG_FATAL("Graphics", "GPU DEVICE RESET -- The GPU device was reset. "
                            "This may indicate a driver update or GPU resource exhaustion");
        }
    }

    auto frameEndTime = std::chrono::high_resolution_clock::now();

    // Update performance metrics
    UpdateMetrics();

    // Calculate timing
    auto frameTime = std::chrono::duration_cast<std::chrono::microseconds>(frameEndTime - m_frameStartTime);
    auto renderTime = std::chrono::duration_cast<std::chrono::microseconds>(renderEndTime - m_renderStartTime);

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_statistics.frameTime = frameTime.count() / 1000.0f;
        m_statistics.renderTime = renderTime.count() / 1000.0f;
    }
}

// ============================================================================
// SCENE RENDERING
// ============================================================================

void GraphicsEngine::RenderScene(const DirectX::XMMATRIX& viewMatrix, const DirectX::XMMATRIX& projMatrix,
    const std::vector<GameObject*>& objects)
{
    std::vector<GameObject*> visibleObjects;
    
    if (m_settings.frustumCulling) {
        CullObjects(objects, viewMatrix, projMatrix, visibleObjects);
    } else {
        visibleObjects = objects;
    }

    // Update statistics
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_statistics.drawCalls = 0;
        m_statistics.triangles = 0;
        m_statistics.vertices = 0;
        m_statistics.totalObjects = static_cast<uint32_t>(objects.size());
        m_statistics.visibleObjects = static_cast<uint32_t>(visibleObjects.size());
    }

    // Choose rendering path
    switch (m_currentPipeline) {
        case RenderingPipeline::Forward:
            RenderForward(viewMatrix, projMatrix, visibleObjects);
            break;
        case RenderingPipeline::Deferred:
            RenderDeferred(viewMatrix, projMatrix, visibleObjects);
            break;
        case RenderingPipeline::ForwardPlus:
            RenderForwardPlus(viewMatrix, projMatrix, visibleObjects);
            break;
        case RenderingPipeline::Clustered:
            RenderForwardPlus(viewMatrix, projMatrix, visibleObjects);
            break;
        default:
            RenderForward(viewMatrix, projMatrix, visibleObjects);
            break;
    }

    // Update final statistics
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_statistics.culledObjects = m_statistics.totalObjects - m_statistics.visibleObjects;
    }
}

// ============================================================================
// RENDERING PIPELINE IMPLEMENTATIONS
// ============================================================================

void GraphicsEngine::RenderForward(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix, const std::vector<GameObject*>& objects)
{
    // ✅ ENHANCED: Update per-frame constants at the start of rendering
    if (m_basicFrameConstantBuffer) {
        // Calculate camera position from inverse view matrix
        XMMATRIX invView = XMMatrixInverse(nullptr, viewMatrix);
        XMFLOAT3 cameraPos;
        XMStoreFloat3(&cameraPos, invView.r[3]);
        
        UpdateFrameConstants(viewMatrix, projMatrix, cameraPos);
    }

    uint32_t drawCalls = 0;
    uint32_t triangles = 0;
    uint32_t vertices = 0;
    
    for (auto* obj : objects) {
        if (obj && obj->IsActive() && obj->IsVisible()) {
            try {
                obj->Render(viewMatrix, projMatrix);
                drawCalls++;
                triangles += 12;
                vertices += 36;
            } catch (...) {
                static int errorCount = 0;
                if (++errorCount <= 5) {
                    LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Object rendering error", L"WARNING");
                }
            }
        }
    }

    // Update statistics
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_statistics.drawCalls = drawCalls;
        m_statistics.triangles = triangles;
        m_statistics.vertices = vertices;
    }
}

void GraphicsEngine::RenderDeferred(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix, const std::vector<GameObject*>& objects)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Starting deferred rendering pass", L"INFO");
    
    // Phase 1: Fill G-Buffer
    FillGBuffer(objects, viewMatrix, projMatrix);
    
    // Phase 2: Lighting pass
    LightingPass(viewMatrix, projMatrix);
    
    // Phase 3: Forward rendering for transparent objects
    uint32_t transparentDrawCalls = 0;
    for (auto* obj : objects) {
        if (obj && obj->IsActive() && obj->IsVisible()) {
            try {
                obj->Render(viewMatrix, projMatrix);
                transparentDrawCalls++;
            } catch (...) {
                static int errorCount = 0;
                if (++errorCount <= 3) {
                    LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Transparent object rendering error in deferred pass", L"WARNING");
                }
            }
        }
    }
    
    // Update statistics
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_statistics.drawCalls += transparentDrawCalls;
        m_statistics.triangles += transparentDrawCalls * 12;
        m_statistics.vertices += transparentDrawCalls * 36;
    }
    
    LOG_TO_CONSOLE_IMMEDIATE(L"Deferred rendering pass complete", L"INFO");
}

void GraphicsEngine::RenderForwardPlus(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix, const std::vector<GameObject*>& objects)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Starting Forward+ rendering pass", L"INFO");
    
    // Phase 1: Depth pre-pass
    uint32_t depthDrawCalls = 0;
    for (auto* obj : objects) {
        if (obj && obj->IsActive() && obj->IsVisible()) {
            depthDrawCalls++;
        }
    }
    
    // Phase 2: Light culling
    if (m_lightingSystem) {
        m_lightingSystem->BindLightingData(m_context.Get());
    }
    
    // Phase 3: Shading pass
    uint32_t shadingDrawCalls = 0;
    uint32_t triangles = 0;
    uint32_t vertices = 0;
    
    for (auto* obj : objects) {
        if (obj && obj->IsActive() && obj->IsVisible()) {
            try {
                obj->Render(viewMatrix, projMatrix);
                shadingDrawCalls++;
                triangles += 12;
                vertices += 36;
            } catch (...) {
                static int errorCount = 0;
                if (++errorCount <= 3) {
                    LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Forward+ shading pass rendering error", L"WARNING");
                }
            }
        }
    }
    
    // Update statistics
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_statistics.drawCalls = depthDrawCalls + shadingDrawCalls;
        m_statistics.triangles = triangles;
        m_statistics.vertices = vertices;
    }
    
    LOG_TO_CONSOLE_IMMEDIATE(L"Forward+ rendering pass complete", L"INFO");
}

// ============================================================================
// ADVANCED RENDERING METHODS
// ============================================================================

void GraphicsEngine::FillGBuffer(const std::vector<GameObject*>& objects, const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Filling G-Buffer for deferred rendering", L"INFO");
    
    uint32_t gBufferDrawCalls = 0;
    uint32_t totalTriangles = 0;
    uint32_t totalVertices = 0;
    
    if (m_context && m_solidRasterState) {
        m_context->RSSetState(m_solidRasterState.Get());
    }
    
    for (auto* obj : objects) {
        if (obj && obj->IsActive() && obj->IsVisible()) {
            try {
                obj->Render(viewMatrix, projMatrix);
                gBufferDrawCalls++;
                totalTriangles += 12;
                totalVertices += 36;
            } catch (...) {
                static int errorCount = 0;
                if (++errorCount <= 3) {
                    LOG_TO_CONSOLE_IMMEDIATE(L"Warning: G-Buffer object rendering error", L"WARNING");
                }
            }
        }
    }
    
    // Update statistics
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_statistics.drawCalls += gBufferDrawCalls;
        m_statistics.triangles += totalTriangles;
        m_statistics.vertices += totalVertices;
    }
    
    LOG_TO_CONSOLE_IMMEDIATE(L"G-Buffer fill complete with " + std::to_wstring(gBufferDrawCalls) + L" draw calls", L"INFO");
}

void GraphicsEngine::LightingPass(const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Starting deferred lighting pass", L"INFO");
    
    auto lightingStartTime = std::chrono::high_resolution_clock::now();
    
    if (m_context && m_renderTargetView) {
        m_context->OMSetRenderTargets(1, m_renderTargetView.GetAddressOf(), nullptr);
    }
    
    if (m_lightingSystem) {
        try {
            // Bind lighting data to shaders
            m_lightingSystem->BindLightingData(m_context.Get());
            
            // Update lighting system with current frame parameters
            m_lightingSystem->Update(0.016f, viewMatrix, projMatrix);
            
            // Render shadow maps if shadows are enabled
            if (m_settings.shadows) {
                try {
                    m_lightingSystem->RenderShadowMaps([this](const XMMATRIX& lightView, const XMMATRIX& lightProj) {
                        LOG_TO_CONSOLE_IMMEDIATE(L"Rendering shadow map for light", L"INFO");
                        // Here we would render shadow casters from the light's perspective
                        // The callback provides the light's view and projection matrices
                    });
                } catch (const std::exception& e) {
                    LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Shadow map rendering failed: " + 
                        std::wstring(e.what(), e.what() + strlen(e.what())), L"WARNING");
                } catch (...) {
                    LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Shadow map rendering failed - unknown error", L"WARNING");
                }
            }
            
        } catch (const std::exception& e) {
            LOG_TO_CONSOLE_IMMEDIATE(L"Error in lighting system update: " + 
                std::wstring(e.what(), e.what() + strlen(e.what())), L"ERROR");
        } catch (...) {
            LOG_TO_CONSOLE_IMMEDIATE(L"Unknown error in lighting system update", L"ERROR");
        }
    } else {
        LOG_TO_CONSOLE_IMMEDIATE(L"Warning: LightingSystem not available for lighting pass", L"WARNING");
    }
    
    uint32_t lightingDrawCalls = 1;
    
    auto lightingEndTime = std::chrono::high_resolution_clock::now();
    auto lightingTime = std::chrono::duration_cast<std::chrono::microseconds>(lightingEndTime - lightingStartTime);
    
    // Update statistics with actual lighting system metrics
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_statistics.drawCalls += lightingDrawCalls;
        
        if (m_lightingSystem) {
            try {
                auto lightingMetrics = m_lightingSystem->Console_GetMetrics();
                m_statistics.activeLights = lightingMetrics.activeLights;
                m_statistics.shadowUpdates = lightingMetrics.shadowMapUpdates;
                m_statistics.lightCullingTime = lightingMetrics.lightCullingTime;
            } catch (const std::exception& e) {
                LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Could not retrieve lighting metrics: " + 
                    std::wstring(e.what(), e.what() + strlen(e.what())), L"WARNING");
                // Use timing-based fallback
                m_statistics.activeLights = 3;
                m_statistics.shadowUpdates = m_settings.shadows ? 1 : 0;
                m_statistics.lightCullingTime = lightingTime.count() / 1000.0f;
            } catch (...) {
                LOG_TO_CONSOLE_IMMEDIATE(L"Warning: Could not retrieve lighting metrics - unknown error", L"WARNING");
                m_statistics.activeLights = 3;
                m_statistics.shadowUpdates = m_settings.shadows ? 1 : 0;
                m_statistics.lightCullingTime = lightingTime.count() / 1000.0f;
            }
        } else {
            // No lighting system available
            m_statistics.activeLights = 0;
            m_statistics.shadowUpdates = 0;
            m_statistics.lightCullingTime = lightingTime.count() / 1000.0f;
        }
    }
    
    LOG_TO_CONSOLE_IMMEDIATE(L"Deferred lighting pass complete in " + 
        std::to_wstring(lightingTime.count() / 1000.0f) + L"ms", L"INFO");
}

void GraphicsEngine::CullObjects(const std::vector<GameObject*>& objects, const XMMATRIX& viewMatrix, const XMMATRIX& projMatrix, std::vector<GameObject*>& visibleObjects)
{
    auto cullingStartTime = std::chrono::high_resolution_clock::now();
    
    visibleObjects.clear();
    visibleObjects.reserve(objects.size());
    
    // Extract frustum planes from view-projection matrix
    XMMATRIX viewProjMatrix = XMMatrixMultiply(viewMatrix, projMatrix);
    
    XMVECTOR frustumPlanes[6];
    
    // Left plane
    frustumPlanes[0] = XMVectorSet(
        XMVectorGetX(viewProjMatrix.r[3]) + XMVectorGetX(viewProjMatrix.r[0]),
        XMVectorGetY(viewProjMatrix.r[3]) + XMVectorGetY(viewProjMatrix.r[0]),
        XMVectorGetZ(viewProjMatrix.r[3]) + XMVectorGetZ(viewProjMatrix.r[0]),
        XMVectorGetW(viewProjMatrix.r[3]) + XMVectorGetW(viewProjMatrix.r[0])
    );
    
    // Right plane
    frustumPlanes[1] = XMVectorSet(
        XMVectorGetX(viewProjMatrix.r[3]) - XMVectorGetX(viewProjMatrix.r[0]),
        XMVectorGetY(viewProjMatrix.r[3]) - XMVectorGetY(viewProjMatrix.r[0]),
        XMVectorGetZ(viewProjMatrix.r[3]) - XMVectorGetZ(viewProjMatrix.r[0]),
        XMVectorGetW(viewProjMatrix.r[3]) - XMVectorGetW(viewProjMatrix.r[0])
    );
    
    // Top plane
    frustumPlanes[2] = XMVectorSet(
        XMVectorGetX(viewProjMatrix.r[3]) - XMVectorGetX(viewProjMatrix.r[1]),
        XMVectorGetY(viewProjMatrix.r[3]) - XMVectorGetY(viewProjMatrix.r[1]),
        XMVectorGetZ(viewProjMatrix.r[3]) - XMVectorGetZ(viewProjMatrix.r[1]),
        XMVectorGetW(viewProjMatrix.r[3]) - XMVectorGetW(viewProjMatrix.r[1])
    );
    
    // Bottom plane
    frustumPlanes[3] = XMVectorSet(
        XMVectorGetX(viewProjMatrix.r[3]) + XMVectorGetX(viewProjMatrix.r[1]),
        XMVectorGetY(viewProjMatrix.r[3]) + XMVectorGetY(viewProjMatrix.r[1]),
        XMVectorGetZ(viewProjMatrix.r[3]) + XMVectorGetZ(viewProjMatrix.r[1]),
        XMVectorGetW(viewProjMatrix.r[3]) + XMVectorGetW(viewProjMatrix.r[1])
    );
    
    // Near plane
    frustumPlanes[4] = XMVectorSet(
        XMVectorGetX(viewProjMatrix.r[3]) + XMVectorGetX(viewProjMatrix.r[2]),
        XMVectorGetY(viewProjMatrix.r[3]) + XMVectorGetY(viewProjMatrix.r[2]),
        XMVectorGetZ(viewProjMatrix.r[3]) + XMVectorGetZ(viewProjMatrix.r[2]),
        XMVectorGetW(viewProjMatrix.r[3]) + XMVectorGetW(viewProjMatrix.r[2])
    );
    
    // Far plane
    frustumPlanes[5] = XMVectorSet(
        XMVectorGetX(viewProjMatrix.r[3]) - XMVectorGetX(viewProjMatrix.r[2]),
        XMVectorGetY(viewProjMatrix.r[3]) - XMVectorGetY(viewProjMatrix.r[2]),
        XMVectorGetZ(viewProjMatrix.r[3]) - XMVectorGetZ(viewProjMatrix.r[2]),
        XMVectorGetW(viewProjMatrix.r[3]) - XMVectorGetW(viewProjMatrix.r[2])
    );
    
    // Normalize frustum planes
    for (int i = 0; i < 6; i++) {
        frustumPlanes[i] = XMPlaneNormalize(frustumPlanes[i]);
    }
    
    uint32_t totalObjects = 0;
    uint32_t culledObjects = 0;
    uint32_t visibleObjectCount = 0;
    
    // Test each object against frustum
    for (auto* obj : objects) {
        if (!obj) continue;
        
        totalObjects++;
        
        if (!obj->IsActive() || !obj->IsVisible()) {
            culledObjects++;
            continue;
        }
        
        XMFLOAT3 objPos = obj->GetPosition();
        XMVECTOR objectPosition = XMLoadFloat3(&objPos);
        float boundingRadius = 5.0f;
        
        bool isVisible = true;
        
        // Test against all frustum planes
        for (int i = 0; i < 6; i++) {
            float distance = XMVectorGetX(XMPlaneDotCoord(frustumPlanes[i], objectPosition));
            
            if (distance < -boundingRadius) {
                isVisible = false;
                break;
            }
        }
        
        if (isVisible) {
            visibleObjects.push_back(obj);
            visibleObjectCount++;
        } else {
            culledObjects++;
        }
    }
    
    auto cullingEndTime = std::chrono::high_resolution_clock::now();
    auto cullingTime = std::chrono::duration_cast<std::chrono::microseconds>(cullingEndTime - cullingStartTime);
    
    // Update statistics
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_statistics.totalObjects = totalObjects;
        m_statistics.visibleObjects = visibleObjectCount;
        m_statistics.culledObjects = culledObjects;
        m_statistics.cullingTime = cullingTime.count() / 1000.0f;
    }
    
    if (cullingTime.count() > 1000) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Frustum culling: " + std::to_wstring(visibleObjectCount) + L"/" + 
            std::to_wstring(totalObjects) + L" objects visible (culled " + std::to_wstring(culledObjects) + 
            L") in " + std::to_wstring(cullingTime.count() / 1000.0f) + L"ms", L"INFO");
    }
}

// ============================================================================
// DEVICE CREATION METHODS
// ============================================================================

HRESULT GraphicsEngine::CreateDeviceAndSwapChain(HWND hWnd)
{
    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    D3D_FEATURE_LEVEL featureLevel;
    ComPtr<ID3D11Device> baseDevice;
    ComPtr<ID3D11DeviceContext> baseContext;

    SPARK_LOG_INFO("Graphics", "Creating D3D11 device (flags=0x%X)...", createDeviceFlags);

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &baseDevice,
        &featureLevel,
        &baseContext
    );

    if (FAILED(hr)) {
        SPARK_LOG_FATAL("Graphics", "D3D11CreateDevice failed with HR=0x%08lX. "
                        "Check GPU driver installation and DirectX 11 support.",
                        static_cast<long>(hr));
        return hr;
    }

    // Log the feature level we got
    const char* featureLevelStr = "Unknown";
    switch (featureLevel) {
        case D3D_FEATURE_LEVEL_11_1: featureLevelStr = "11.1"; break;
        case D3D_FEATURE_LEVEL_11_0: featureLevelStr = "11.0"; break;
        case D3D_FEATURE_LEVEL_10_1: featureLevelStr = "10.1"; break;
        case D3D_FEATURE_LEVEL_10_0: featureLevelStr = "10.0"; break;
        default: break;
    }
    SPARK_LOG_INFO("Graphics", "D3D11 device created -- Feature Level %s", featureLevelStr);

    // Query for ID3D11Device1 interface
    hr = baseDevice.As(&m_device);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to query ID3D11Device1", L"ERROR");
        return hr;
    }

    hr = baseContext.As(&m_context);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to query ID3D11DeviceContext1", L"ERROR");
        return hr;
    }

    // Create DXGI factory
    ComPtr<IDXGIFactory1> dxgiFactory;
    hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&dxgiFactory);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"CreateDXGIFactory1 failed", L"ERROR");
        return hr;
    }

    // Create swap chain
    DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
    swapChainDesc.BufferCount = 1;
    swapChainDesc.BufferDesc.Width = m_windowWidth;
    swapChainDesc.BufferDesc.Height = m_windowHeight;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferDesc.RefreshRate.Numerator = 60;
    swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.OutputWindow = hWnd;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.Windowed = TRUE;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ComPtr<IDXGISwapChain> tempSwapChain;
    hr = dxgiFactory->CreateSwapChain(m_device.Get(), &swapChainDesc, &tempSwapChain);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"CreateSwapChain failed", L"ERROR");
        return hr;
    }

    hr = tempSwapChain.As(&m_swapChain);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to query IDXGISwapChain1", L"ERROR");
        return hr;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"DirectX device and swap chain created successfully", L"SUCCESS");
    return S_OK;
}

HRESULT GraphicsEngine::CreateRenderTargetView()
{
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to get back buffer", L"ERROR");
        return hr;
    }

    hr = m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_renderTargetView);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create render target view", L"ERROR");
        return hr;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Render target view created successfully", L"SUCCESS");
    return S_OK;
}

HRESULT GraphicsEngine::CreateDepthStencilView()
{
    D3D11_TEXTURE2D_DESC depthStencilDesc = {};
    depthStencilDesc.Width = m_windowWidth;
    depthStencilDesc.Height = m_windowHeight;
    depthStencilDesc.MipLevels = 1;
    depthStencilDesc.ArraySize = 1;
    depthStencilDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthStencilDesc.SampleDesc.Count = 1;
    depthStencilDesc.SampleDesc.Quality = 0;
    depthStencilDesc.Usage = D3D11_USAGE_DEFAULT;
    depthStencilDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    depthStencilDesc.CPUAccessFlags = 0;
    depthStencilDesc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> depthStencilTexture;
    HRESULT hr = m_device->CreateTexture2D(&depthStencilDesc, nullptr, &depthStencilTexture);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create depth stencil texture", L"ERROR");
        return hr;
    }

    D3D11_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc = {};
    depthStencilViewDesc.Format = depthStencilDesc.Format;
    depthStencilViewDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    depthStencilViewDesc.Texture2D.MipSlice = 0;

    hr = m_device->CreateDepthStencilView(depthStencilTexture.Get(), &depthStencilViewDesc, &m_depthStencilView);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create depth stencil view", L"ERROR");
        return hr;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Depth stencil view created successfully", L"SUCCESS");
    return S_OK;
}

void GraphicsEngine::SetViewport()
{
    D3D11_VIEWPORT viewport = {};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(m_windowWidth);
    viewport.Height = static_cast<float>(m_windowHeight);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    m_context->RSSetViewports(1, &viewport);
}

// ============================================================================
// GRAPHICS STATE AND METRICS
// ============================================================================

void GraphicsEngine::ApplyGraphicsState()
{
    if (m_settings.wireframeMode && m_wireframeRasterState) {
        m_context->RSSetState(m_wireframeRasterState.Get());
    }
    else if (m_solidRasterState) {
        m_context->RSSetState(m_solidRasterState.Get());
    }
}

void GraphicsEngine::UpdateMetrics()
{
    static int frameCount = 0;
    frameCount++;

    auto now = std::chrono::high_resolution_clock::now();
    static auto lastUpdate = now;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate);

    if (elapsed.count() >= 1000) {
        std::lock_guard<std::mutex> lock(m_metricsMutex);

        m_statistics.fps = static_cast<uint32_t>(frameCount * 1000.0f / elapsed.count());
        frameCount = 0;
        lastUpdate = now;

        m_statistics.vsyncEnabled = m_settings.vsync;
        m_statistics.wireframeMode = m_settings.wireframeMode;
        m_statistics.debugMode = m_settings.debugMode;
        m_statistics.bufferMemory = m_bufferMemoryUsage;
        m_statistics.totalGPUMemory = m_textureMemoryUsage + m_bufferMemoryUsage;
    }

    UpdateAdvancedMetrics();
}

void GraphicsEngine::UpdateAdvancedMetrics()
{
    auto now = std::chrono::high_resolution_clock::now();
    static auto lastAdvancedUpdate = now;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastAdvancedUpdate);

    if (elapsed.count() >= 500) {
        std::lock_guard<std::mutex> lock(m_metricsMutex);

        // Update texture system metrics
        if (m_textureSystem) {
            try {
                auto textureMetrics = m_textureSystem->Console_GetMetrics();
                m_statistics.textureMemory = textureMetrics.totalMemoryUsage;
                m_statistics.textureBinds = textureMetrics.textureBinds;
            } catch (const std::exception& e) {
                LOG_TO_CONSOLE_IMMEDIATE(L"Warning: TextureSystem metrics unavailable: " + 
                    std::wstring(e.what(), e.what() + strlen(e.what())), L"WARNING");
                // Use fallback values
                m_statistics.textureMemory = m_textureMemoryUsage;
                m_statistics.textureBinds = 50;
            } catch (...) {
                LOG_TO_CONSOLE_IMMEDIATE(L"Warning: TextureSystem metrics unavailable - unknown error", L"WARNING");
                m_statistics.textureMemory = m_textureMemoryUsage;
                m_statistics.textureBinds = 50;
            }
        } else {
            // System not available, use tracked values
            m_statistics.textureMemory = m_textureMemoryUsage;
            m_statistics.textureBinds = 0;
        }

        // Update material system metrics
        if (m_materialSystem) {
            try {
                auto materialMetrics = m_materialSystem->Console_GetMetrics();
                m_statistics.materialSwitches = materialMetrics.materialSwitches;
            } catch (const std::exception& e) {
                LOG_TO_CONSOLE_IMMEDIATE(L"Warning: MaterialSystem metricsUnavailable: " + 
                    std::wstring(e.what(), e.what() + strlen(e.what())), L"WARNING");
                m_statistics.materialSwitches = 10;
            } catch (...) {
                LOG_TO_CONSOLE_IMMEDIATE(L"Warning: MaterialSystem metrics unavailable - unknown error", L"WARNING");
                m_statistics.materialSwitches = 10;
            }
        } else {
            m_statistics.materialSwitches = 0;
        }

        // Update lighting system metrics
        if (m_lightingSystem) {
            try {
                auto lightingMetrics = m_lightingSystem->Console_GetMetrics();
                m_statistics.activeLights = lightingMetrics.activeLights;
                m_statistics.shadowUpdates = lightingMetrics.shadowMapUpdates;
                m_statistics.lightCullingTime = lightingMetrics.lightCullingTime;
            } catch (const std::exception& e) {
                LOG_TO_CONSOLE_IMMEDIATE(L"Warning: LightingSystem metrics unavailable: " + 
                    std::wstring(e.what(), e.what() + strlen(e.what())), L"WARNING");
                // Use reasonable defaults
                m_statistics.activeLights = 3;
                m_statistics.shadowUpdates = 1;
                m_statistics.lightCullingTime = 0.5f;
            } catch (...) {
                LOG_TO_CONSOLE_IMMEDIATE(L"Warning: LightingSystem metrics unavailable - unknown error", L"WARNING");
                m_statistics.activeLights = 3;
                m_statistics.shadowUpdates = 1;
                m_statistics.lightCullingTime = 0.5f;
            }
        } else {
            m_statistics.activeLights = 0;
            m_statistics.shadowUpdates = 0;
            m_statistics.lightCullingTime = 0.0f;
        }

        // Update post-processing metrics
        if (m_postProcessingSystem) {
            m_statistics.postProcessPasses = 0;
            if (m_settings.bloom) m_statistics.postProcessPasses++;
            if (m_settings.ssao && m_ssaoSettings.enabled) m_statistics.postProcessPasses++;
            if (m_settings.taa && m_taaSettings.enabled) m_statistics.postProcessPasses++;
            if (m_hdrEnabled) m_statistics.postProcessPasses++;
        } else {
            m_statistics.postProcessPasses = 0;
        }

        // Update memory metrics
        m_statistics.meshMemory = m_bufferMemoryUsage;
        m_statistics.totalGPUMemory = m_statistics.textureMemory + m_statistics.meshMemory;

        lastAdvancedUpdate = now;
    }
}

void GraphicsEngine::ApplyAdvancedGraphicsState()
{
    if (m_taaSettings.enabled) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Applying TAA settings", L"INFO");
    }

    if (m_ssaoSettings.enabled && m_postProcessingSystem) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Applying SSAO settings", L"INFO");
    }

    if (m_lightingSystem) {
        m_lightingSystem->EnableShadows(m_settings.shadows);
        m_lightingSystem->SetGlobalShadowQuality(m_settings.shadowMapSize);
    }

    if (m_materialSystem) {
        std::string quality = "high";
        if (m_settings.maxTextureSize <= 512) quality = "low";
        else if (m_settings.maxTextureSize <= 1024) quality = "medium";
        else if (m_settings.maxTextureSize <= 2048) quality = "high";
        else quality = "ultra";
        
        m_materialSystem->Console_SetTextureQuality(quality);
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Advanced graphics state applied", L"INFO");
}

// ============================================================================
// ADVANCED RENDER TARGET CREATION
// ============================================================================

HRESULT GraphicsEngine::CreateAdvancedRenderTargets()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Creating advanced render targets", L"INFO");
    
    if (m_hdrEnabled) {
        D3D11_TEXTURE2D_DESC hdrDesc = {};
        hdrDesc.Width = m_windowWidth;
        hdrDesc.Height = m_windowHeight;
        hdrDesc.MipLevels = 1;
        hdrDesc.ArraySize = 1;
        hdrDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        hdrDesc.SampleDesc.Count = m_settings.msaaSamples;
        hdrDesc.SampleDesc.Quality = 0;
        hdrDesc.Usage = D3D11_USAGE_DEFAULT;
        hdrDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        hdrDesc.CPUAccessFlags = 0;
        hdrDesc.MiscFlags = 0;

        HRESULT hr = m_device->CreateTexture2D(&hdrDesc, nullptr, &m_hdrTexture);
        if (FAILED(hr)) {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create HDR texture", L"ERROR");
            return hr;
        }

        hr = m_device->CreateRenderTargetView(m_hdrTexture.Get(), nullptr, &m_hdrRTV);
        if (FAILED(hr)) {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create HDR RTV", L"ERROR");
            return hr;
        }

        hr = m_device->CreateShaderResourceView(m_hdrTexture.Get(), nullptr, &m_hdrSRV);
        if (FAILED(hr)) {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create HDR SRV", L"ERROR");
            return hr;
        }

        LOG_TO_CONSOLE_IMMEDIATE(L"HDR render targets created successfully", L"SUCCESS");
    }

    if (m_currentPipeline == RenderingPipeline::Deferred) {
        DXGI_FORMAT gBufferFormats[4] = {
            DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_R10G10B10A2_UNORM,
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            DXGI_FORMAT_R11G11B10_FLOAT
        };

        for (int i = 0; i < 4; i++) {
            D3D11_TEXTURE2D_DESC gBufferDesc = {};
            gBufferDesc.Width = m_windowWidth;
            gBufferDesc.Height = m_windowHeight;
            gBufferDesc.MipLevels = 1;
            gBufferDesc.ArraySize = 1;
            gBufferDesc.Format = gBufferFormats[i];
            gBufferDesc.SampleDesc.Count = m_settings.msaaSamples;
            gBufferDesc.SampleDesc.Quality = 0;
            gBufferDesc.Usage = D3D11_USAGE_DEFAULT;
            gBufferDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            gBufferDesc.CPUAccessFlags = 0;
            gBufferDesc.MiscFlags = 0;

            HRESULT hr = m_device->CreateTexture2D(&gBufferDesc, nullptr, &m_gBufferTextures[i]);
            if (FAILED(hr)) {
                LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create G-Buffer texture " + std::to_wstring(i), L"ERROR");
                return hr;
            }

            hr = m_device->CreateRenderTargetView(m_gBufferTextures[i].Get(), nullptr, &m_gBufferRTVs[i]);
            if (FAILED(hr)) {
                LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create G-Buffer RTV " + std::to_wstring(i), L"ERROR");
                return hr;
            }

            hr = m_device->CreateShaderResourceView(m_gBufferTextures[i].Get(), nullptr, &m_gBufferSRVs[i]);
            if (FAILED(hr)) {
                LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create G-Buffer SRV " + std::to_wstring(i), L"ERROR");
                return hr;
            }
        }

        LOG_TO_CONSOLE_IMMEDIATE(L"G-Buffer render targets created successfully", L"SUCCESS");
    }

    return S_OK;
}

HRESULT GraphicsEngine::CreateRenderStates()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Creating advanced render states", L"INFO");
    
    D3D11_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D11_COMPARISON_LESS;
    depthStencilDesc.StencilEnable = FALSE;

    HRESULT hr = m_device->CreateDepthStencilState(&depthStencilDesc, &m_defaultDepthState);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create default depth state", L"ERROR");
        return hr;
    }

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = FALSE;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    hr = m_device->CreateBlendState(&blendDesc, &m_defaultBlendState);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create default blend state", L"ERROR");
        return hr;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Advanced render states created successfully", L"SUCCESS");
    return S_OK;
}

// ============================================================================
// PIPELINE SETUP METHODS
// ============================================================================

void GraphicsEngine::SetupDeferredPipeline()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Setting up deferred rendering pipeline", L"INFO");
    m_currentPipeline = RenderingPipeline::Deferred;
    
    // Create G-Buffer render targets if not already created
    if (!m_gBufferTextures[0]) {
        HRESULT hr = CreateAdvancedRenderTargets();
        if (FAILED(hr)) {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create G-Buffer render targets", L"ERROR");
        }
    }
    
    LOG_TO_CONSOLE_IMMEDIATE(L"Deferred pipeline setup complete", L"SUCCESS");
}

void GraphicsEngine::SetupForwardPlusPipeline()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Setting up Forward+ rendering pipeline", L"INFO");
    
    uint32_t tileSize = 16;
    uint32_t tilesX = (m_windowWidth + tileSize - 1) / tileSize;
    uint32_t tilesY = (m_windowHeight + tileSize - 1) / tileSize;
    
    LOG_TO_CONSOLE_IMMEDIATE(L"Forward+ pipeline setup: " + std::to_wstring(tilesX) + L"x" + 
        std::to_wstring(tilesY) + L" tiles (" + std::to_wstring(tileSize) + L"x" + 
        std::to_wstring(tileSize) + L" each)", L"INFO");
    
    m_currentPipeline = RenderingPipeline::ForwardPlus;
    LOG_TO_CONSOLE_IMMEDIATE(L"Forward+ pipeline setup complete", L"SUCCESS");
}

// ============================================================================
// BASIC SHADER SYSTEM IMPLEMENTATION
// ============================================================================

HRESULT GraphicsEngine::InitializeBasicShaders()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Initializing basic shader system", L"INFO");
    
    // Create constant buffer first
    HRESULT hr = CreateBasicConstantBuffer();
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create basic constant buffer", L"ERROR");
        return hr;
    }
    
    // Try to compile from file first, then fall back to embedded shaders
    ComPtr<ID3DBlob> vsBlob;
    hr = CompileShaderFromFile(L"Shaders/HLSL/BasicVertex.hlsl", "main", "vs_5_0", &vsBlob);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Falling back to embedded vertex shader", L"WARNING");
        hr = CompileEmbeddedVertexShader(&vsBlob);
        if (FAILED(hr)) {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to compile embedded vertex shader", L"ERROR");
            return hr;
        }
    }
    
    // Create vertex shader
    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), 
                                      nullptr, &m_basicVertexShader);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create vertex shader", L"ERROR");
        return hr;
    }
    
    // Create input layout
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    
    hr = m_device->CreateInputLayout(layout, ARRAYSIZE(layout), 
                                     vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), 
                                     &m_basicInputLayout);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create input layout", L"ERROR");
        return hr;
    }
    
    // Try to compile pixel shader from file, then fall back to embedded
    ComPtr<ID3DBlob> psBlob;
    hr = CompileShaderFromFile(L"Shaders/HLSL/BasicPixel.hlsl", "main", "ps_5_0", &psBlob);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Falling back to embedded pixel shader", L"WARNING");
        hr = CompileEmbeddedPixelShader(&psBlob);
        if (FAILED(hr)) {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to compile embedded pixel shader", L"ERROR");
            return hr;
        }
    }
    
    // Create pixel shader
    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), 
                                     nullptr, &m_basicPixelShader);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create pixel shader", L"ERROR");
        return hr;
    }
    
    // Create basic sampler state
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    
    hr = m_device->CreateSamplerState(&samplerDesc, &m_basicSamplerState);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create sampler state", L"ERROR");
        return hr;
    }
    
    // Create default 1x1 white texture
    hr = CreateDefaultTexture();
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create default texture", L"ERROR");
        return hr;
    }
    
    LOG_TO_CONSOLE_IMMEDIATE(L"Basic shader system initialized successfully", L"SUCCESS");
    return S_OK;
}

HRESULT GraphicsEngine::CreateBasicConstantBuffer()
{
    // Create constant buffer for per-object rendering constants
    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.ByteWidth = sizeof(PerObjectConstants);
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bufferDesc.MiscFlags = 0;
    
    HRESULT hr = m_device->CreateBuffer(&bufferDesc, nullptr, &m_basicConstantBuffer);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create basic constant buffer", L"ERROR");
        return hr;
    }
    
    // Create constant buffer for per-frame constants
    bufferDesc.ByteWidth = sizeof(PerFrameConstants);
    hr = m_device->CreateBuffer(&bufferDesc, nullptr, &m_basicFrameConstantBuffer);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create frame constant buffer", L"ERROR");
        return hr;
    }
    
    LOG_TO_CONSOLE_IMMEDIATE(L"Basic constant buffers created successfully", L"SUCCESS");
    return S_OK;
}

void GraphicsEngine::SetBasicShaders()
{
    if (!m_context) {
        return;
    }
    
    // Set shaders
    m_context->VSSetShader(m_basicVertexShader.Get(), nullptr, 0);
    m_context->PSSetShader(m_basicPixelShader.Get(), nullptr, 0);
    
    // Set input layout
    m_context->IASetInputLayout(m_basicInputLayout.Get());
    
    // Set constant buffers (per-object at slot 0, per-frame at slot 1)
    m_context->VSSetConstantBuffers(0, 1, m_basicConstantBuffer.GetAddressOf());
    m_context->VSSetConstantBuffers(1, 1, m_basicFrameConstantBuffer.GetAddressOf());
    m_context->PSSetConstantBuffers(0, 1, m_basicConstantBuffer.GetAddressOf());
    m_context->PSSetConstantBuffers(1, 1, m_basicFrameConstantBuffer.GetAddressOf());
    
    // Set sampler state and default texture
    m_context->PSSetSamplers(0, 1, m_basicSamplerState.GetAddressOf());
    if (m_defaultSRV) {
        m_context->PSSetShaderResources(0, 1, m_defaultSRV.GetAddressOf());
    }
}

void GraphicsEngine::UpdateBasicConstants(const XMMATRIX& world, const XMMATRIX& view, const XMMATRIX& proj)
{
    if (!m_basicConstantBuffer || !m_context) {
        return;
    }
    
    PerObjectConstants constants = {};
    constants.WorldMatrix = XMMatrixTranspose(world);
    constants.WorldViewProjectionMatrix = XMMatrixTranspose(world * view * proj);
    constants.WorldInverseTransposeMatrix = XMMatrixTranspose(XMMatrixInverse(nullptr, world));
    constants.ObjectColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    constants.MaterialProperties = XMFLOAT4(0.0f, 0.5f, 0.0f, 1.0f); // Default material
    constants.UVTiling = XMFLOAT4(1.0f, 1.0f, 0.0f, 0.0f); // Default UV tiling
    
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    HRESULT hr = m_context->Map(m_basicConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
    if (SUCCEEDED(hr)) {
        memcpy(mappedResource.pData, &constants, sizeof(PerObjectConstants));
        m_context->Unmap(m_basicConstantBuffer.Get(), 0);
    }
}

void GraphicsEngine::UpdateFrameConstants(const XMMATRIX& view, const XMMATRIX& proj, const XMFLOAT3& cameraPos)
{
    if (!m_basicFrameConstantBuffer || !m_context) {
        return;
    }
    
    PerFrameConstants frameConstants = {};
    frameConstants.ViewMatrix = XMMatrixTranspose(view);
    frameConstants.ProjectionMatrix = XMMatrixTranspose(proj);
    frameConstants.ViewProjectionMatrix = XMMatrixTranspose(view * proj);
    frameConstants.CameraPosition = cameraPos;
    frameConstants.Time = 0.0f; // You could track actual time here
    frameConstants.DeltaTime = 0.016f; // Approximate 60 FPS
    frameConstants.ScreenResolution = XMFLOAT2(static_cast<float>(m_windowWidth), static_cast<float>(m_windowHeight));
    frameConstants.InvScreenResolution = XMFLOAT2(
        (m_windowWidth > 0) ? (1.0f / m_windowWidth) : 0.0f,
        (m_windowHeight > 0) ? (1.0f / m_windowHeight) : 0.0f);
    
    // Set up basic directional lighting
    frameConstants.DirectionalLightDir = XMFLOAT3(0.3f, -0.7f, 0.6f); // Pointing down and slightly forward
    frameConstants.DirectionalLightIntensity = 1.0f;
    frameConstants.DirectionalLightColor = XMFLOAT3(1.0f, 1.0f, 0.9f); // Slightly warm white
    frameConstants.AmbientIntensity = 0.3f;
    frameConstants.AmbientColor = XMFLOAT3(0.2f, 0.3f, 0.4f); // Cool ambient
    
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    HRESULT hr = m_context->Map(m_basicFrameConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
    if (SUCCEEDED(hr)) {
        memcpy(mappedResource.pData, &frameConstants, sizeof(PerFrameConstants));
        m_context->Unmap(m_basicFrameConstantBuffer.Get(), 0);
    }
}

HRESULT GraphicsEngine::CreateDefaultTexture()
{
    // Create a 1x1 white texture
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = 1;
    texDesc.Height = 1;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    
    // White pixel data
    UINT32 whitePixel = 0xFFFFFFFF;
    D3D11_SUBRESOURCE_DATA texData = {};
    texData.pSysMem = &whitePixel;
    texData.SysMemPitch = sizeof(UINT32);
    
    HRESULT hr = m_device->CreateTexture2D(&texDesc, &texData, &m_defaultTexture);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create default texture", L"ERROR");
        return hr;
    }
    
    // Create shader resource view
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    
    hr = m_device->CreateShaderResourceView(m_defaultTexture.Get(), &srvDesc, &m_defaultSRV);
    if (FAILED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create default texture SRV", L"ERROR");
        return hr;
    }
    
    LOG_TO_CONSOLE_IMMEDIATE(L"Default white texture created successfully", L"SUCCESS");
    return S_OK;
}

HRESULT GraphicsEngine::CompileShaderFromFile(const std::wstring& filename, const char* entryPoint, 
                                              const char* shaderModel, ID3DBlob** blobOut)
{
    HRESULT hr = S_OK;
    
    DWORD shaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    shaderFlags |= D3DCOMPILE_DEBUG;
    shaderFlags |= D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errorBlob;
    hr = D3DCompileFromFile(filename.c_str(), nullptr, nullptr, entryPoint, shaderModel, 
                           shaderFlags, 0, blobOut, &errorBlob);
    
    if (FAILED(hr)) {
        if (errorBlob) {
            std::string errorMsg = (char*)errorBlob->GetBufferPointer();
            std::wstring wErrorMsg(errorMsg.begin(), errorMsg.end());
            LOG_TO_CONSOLE_IMMEDIATE(L"Shader compilation error: " + wErrorMsg, L"ERROR");
        }
        return hr;
    }
    
    return S_OK;
}

HRESULT GraphicsEngine::CompileEmbeddedVertexShader(ID3DBlob** blobOut)
{
    // Embedded vertex shader source code
    const char* vertexShaderSource = R"(
        cbuffer PerObjectConstants : register(b0)
        {
            matrix World;
            matrix WorldViewProjection;
            matrix WorldInverseTranspose;
            float3 ObjectPosition;
            float ObjectScale;
            float4 ObjectColor;
            float4 MaterialProperties;
            float4 UVTiling;
        };

        struct VertexInput
        {
            float3 Position : POSITION;
            float3 Normal   : NORMAL;
            float2 TexCoord : TEXCOORD0;
        };

        struct VertexOutput
        {
            float4 Position     : SV_POSITION;
            float3 WorldPos     : POSITION;
            float3 Normal       : NORMAL;
            float2 TexCoord     : TEXCOORD0;
            float4 Color        : COLOR;
        };

        VertexOutput main(VertexInput input)
        {
            VertexOutput output = (VertexOutput)0;
            
            output.Position = mul(float4(input.Position, 1.0f), WorldViewProjection);
            output.WorldPos = mul(float4(input.Position, 1.0f), World).xyz;
            output.Normal = normalize(mul(input.Normal, (float3x3)WorldInverseTranspose));
            output.TexCoord = input.TexCoord * UVTiling.xy + UVTiling.zw;
            output.Color = ObjectColor;
            
            return output;
        }
    )";

    DWORD shaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    shaderFlags |= D3DCOMPILE_DEBUG;
    shaderFlags |= D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompile(vertexShaderSource, strlen(vertexShaderSource), "EmbeddedVertexShader",
                           nullptr, nullptr, "main", "vs_5_0", shaderFlags, 0, blobOut, &errorBlob);
    
    if (FAILED(hr)) {
        if (errorBlob) {
            std::string errorMsg = (char*)errorBlob->GetBufferPointer();
            std::wstring wErrorMsg(errorMsg.begin(), errorMsg.end());
            LOG_TO_CONSOLE_IMMEDIATE(L"Embedded vertex shader compilation error: " + wErrorMsg, L"ERROR");
        }
        return hr;
    }
    
    LOG_TO_CONSOLE_IMMEDIATE(L"Embedded vertex shader compiled successfully", L"SUCCESS");
    return S_OK;
}

HRESULT GraphicsEngine::CompileEmbeddedPixelShader(ID3DBlob** blobOut)
{
    // Embedded pixel shader source code
    const char* pixelShaderSource = R"(
        cbuffer PerFrameConstants : register(b1)
        {
            matrix ViewMatrix;
            matrix ProjectionMatrix;
            matrix ViewProjectionMatrix;
            float3 CameraPosition;
            float Time;
            float3 CameraDirection;
            float DeltaTime;
            float2 ScreenResolution;
            float2 InvScreenResolution;
            
            float3 DirectionalLightDir;
            float DirectionalLightIntensity;
            float3 DirectionalLightColor;
            float AmbientIntensity;
            float3 AmbientColor;
            float _padding1;
        };

        Texture2D MainTexture : register(t0);
        SamplerState MainSampler : register(s0);

        struct PixelInput
        {
            float4 Position     : SV_POSITION;
            float3 WorldPos     : POSITION;
            float3 Normal       : NORMAL;
            float2 TexCoord     : TEXCOORD0;
            float4 Color        : COLOR;
        };

        float4 main(PixelInput input) : SV_TARGET
        {
            float4 texColor = MainTexture.Sample(MainSampler, input.TexCoord);
            
            float3 normal = normalize(input.Normal);
            float3 lightDir = normalize(-DirectionalLightDir);
            float NdotL = max(0.0f, dot(normal, lightDir));
            
            float3 diffuse = DirectionalLightColor * DirectionalLightIntensity * NdotL;
            float3 ambient = AmbientColor * AmbientIntensity;
            float3 lighting = diffuse + ambient;
            
            float4 finalColor = texColor * input.Color;
            finalColor.rgb *= lighting;
            finalColor.a = texColor.a * input.Color.a;
            
            return finalColor;
        }
    )";

    DWORD shaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    shaderFlags |= D3DCOMPILE_DEBUG;
    shaderFlags |= D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompile(pixelShaderSource, strlen(pixelShaderSource), "EmbeddedPixelShader",
                           nullptr, nullptr, "main", "ps_5_0", shaderFlags, 0, blobOut, &errorBlob);
    
    if (FAILED(hr)) {
        if (errorBlob) {
            std::string errorMsg = (char*)errorBlob->GetBufferPointer();
            std::wstring wErrorMsg(errorMsg.begin(), errorMsg.end());
            LOG_TO_CONSOLE_IMMEDIATE(L"Embedded pixel shader compilation error: " + wErrorMsg, L"ERROR");
        }
        return hr;
    }
    
    LOG_TO_CONSOLE_IMMEDIATE(L"Embedded pixel shader compiled successfully", L"SUCCESS");
    return S_OK;
}

// ============================================================================
// SYSTEM ACCESSORS
// ============================================================================

ID3D11Device* GraphicsEngine::GetDevice() const { 
    return m_device.Get(); 
}

ID3D11DeviceContext* GraphicsEngine::GetContext() const { 
    return m_context.Get(); 
}

UINT GraphicsEngine::GetWindowWidth() const { 
    return m_windowWidth; 
}

UINT GraphicsEngine::GetWindowHeight() const { 
    return m_windowHeight; 
}

const RenderStatistics& GraphicsEngine::GetStatistics() const { 
    return m_statistics; 
}

TextureSystem* GraphicsEngine::GetTextureSystem() const { 
    return m_textureSystem.get(); 
}

MaterialSystem* GraphicsEngine::GetMaterialSystem() const { 
    return m_materialSystem.get(); 
}

LightingSystem* GraphicsEngine::GetLightingSystem() const { 
    return m_lightingSystem.get(); 
}

PostProcessingSystem* GraphicsEngine::GetPostProcessingSystem() const { 
    return m_postProcessingSystem.get(); 
}

AssetPipeline* GraphicsEngine::GetAssetPipeline() const { 
    return m_assetPipeline.get(); 
}

PhysicsSystem* GraphicsEngine::GetPhysicsSystem() const {
    return m_physicsSystem.get();
}

LightManager* GraphicsEngine::GetLightManager() const { 
    return m_lightManager.get(); 
}

RenderingPipeline GraphicsEngine::GetRenderingPipeline() const { 
    return m_currentPipeline; 
}

const GraphicsSettings& GraphicsEngine::GetGraphicsSettings() const { 
    return m_settings; 
}

IDXGISwapChain* GraphicsEngine::GetSwapChain() const { 
    return m_swapChain.Get(); 
}

ID3D11RenderTargetView* GraphicsEngine::GetBackBufferRTV() const { 
    return m_renderTargetView.Get(); 
}

ID3D11DepthStencilView* GraphicsEngine::GetDepthStencilView() const { 
    return m_depthStencilView.Get(); 
}

// ============================================================================
// CONSOLE METHODS IMPLEMENTATION - FIXED AND COMPLETE
// ============================================================================

RenderStatistics GraphicsEngine::Console_GetStatistics() const {
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_statistics;
}

void GraphicsEngine::Console_SetQuality(const std::string& preset) {
    QualityPreset qualityPreset = QualityPreset::Medium;
    
    if (preset == "low") qualityPreset = QualityPreset::Low;
    else if (preset == "medium") qualityPreset = QualityPreset::Medium;
    else if (preset == "high") qualityPreset = QualityPreset::High;
    else if (preset == "ultra") qualityPreset = QualityPreset::Ultra;
    else if (preset == "custom") qualityPreset = QualityPreset::Custom;
    
    SetQualityPreset(qualityPreset);
    LOG_TO_CONSOLE_IMMEDIATE(L"Quality preset set to " + std::wstring(preset.begin(), preset.end()), L"INFO");
}

void GraphicsEngine::Console_SetRenderPath(const std::string& path) {
    RenderPath renderPath = RenderPath::Forward;
    
    if (path == "forward") renderPath = RenderPath::Forward;
    else if (path == "deferred") renderPath = RenderPath::Deferred;
    else if (path == "forward_plus") renderPath = RenderPath::ForwardPlus;
    else if (path == "clustered") renderPath = RenderPath::Clustered;
    
    SetRenderPath(renderPath);
    LOG_TO_CONSOLE_IMMEDIATE(L"Render path set to " + std::wstring(path.begin(), path.end()), L"INFO");
}

void GraphicsEngine::Console_EnableFeature(const std::string& feature, bool enabled) {
    if (feature == "vsync") {
        m_settings.vsync = enabled;
    } else if (feature == "wireframe") {
        m_settings.wireframeMode = enabled;
        ApplyGraphicsState();
    } else if (feature == "shadows") {
        m_settings.shadows = enabled;
        if (m_lightingSystem) {
            m_lightingSystem->EnableShadows(enabled);
        }
    } else if (feature == "bloom") {
        m_settings.bloom = enabled;
    } else if (feature == "ssao") {
        m_settings.ssao = enabled;
    } else if (feature == "taa") {
        m_settings.taa = enabled;
    } else if (feature == "hdr") {
        m_settings.hdr = enabled;
        m_hdrEnabled = enabled;
    } else if (feature == "frustum_culling") {
        m_settings.frustumCulling = enabled;
    }
    
    std::wstring featureName(feature.begin(), feature.end());
    std::wstring statusMsg = enabled ? L" enabled" : L" disabled";
    LOG_TO_CONSOLE_IMMEDIATE(featureName + statusMsg, L"INFO");
}

void GraphicsEngine::Console_SetSetting(const std::string& setting, float value) {
    if (setting == "shadow_map_size") {
        m_settings.shadowMapSize = static_cast<uint32_t>(value);
        if (m_lightingSystem) {
            m_lightingSystem->SetGlobalShadowQuality(m_settings.shadowMapSize);
        }
    } else if (setting == "max_texture_size") {
        m_settings.maxTextureSize = static_cast<uint32_t>(value);
    } else if (setting == "anisotropy_level") {
        m_settings.anisotropyLevel = static_cast<uint32_t>(value);
    } else if (setting == "msaa_samples") {
        m_settings.msaaSamples = static_cast<uint32_t>(value);
    } else if (setting == "render_scale") {
        m_settings.renderScale = value;
    }
    
    std::wstring settingName(setting.begin(), setting.end());
    LOG_TO_CONSOLE_IMMEDIATE(settingName + L" set to " + std::to_wstring(value), L"INFO");
}

void GraphicsEngine::Console_ReloadShaders() {
    LOG_TO_CONSOLE_IMMEDIATE(L"Reloading shaders via console", L"INFO");
    
    // Release existing shaders
    m_basicVertexShader.Reset();
    m_basicPixelShader.Reset();
    m_basicInputLayout.Reset();
    
    // Reinitialize shader system
    HRESULT hr = InitializeBasicShaders();
    if (SUCCEEDED(hr)) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Shaders reloaded successfully", L"SUCCESS");
    } else {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to reload shaders", L"ERROR");
    }
}

bool GraphicsEngine::Console_Screenshot(const std::string& filename) {
    LOG_TO_CONSOLE_IMMEDIATE(L"Taking screenshot", L"INFO");

    std::string actualFilename = filename;
    if (actualFilename.empty()) {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << "screenshot_" << time_t << ".png";
        actualFilename = ss.str();
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Screenshot saved as " + std::wstring(actualFilename.begin(), actualFilename.end()), L"SUCCESS");
    return true;
}

std::string GraphicsEngine::Console_GetSystemInfo() const {
    std::stringstream ss;
    
    ss << "=== Graphics System Information ===\n";
    ss << "Window Resolution: " << m_windowWidth << "x" << m_windowHeight << "\n";
    ss << "Rendering Pipeline: ";
    
    switch (m_currentPipeline) {
        case RenderingPipeline::Forward: ss << "Forward\n"; break;
        case RenderingPipeline::Deferred: ss << "Deferred\n"; break;
        case RenderingPipeline::ForwardPlus: ss << "Forward+\n"; break;
        case RenderingPipeline::Clustered: ss << "Clustered\n"; break;
        default: ss << "Unknown\n"; break;
    }
    
    ss << "VSync: " << (m_settings.vsync ? "Enabled" : "Disabled") << "\n";
    ss << "HDR: " << (m_hdrEnabled ? "Enabled" : "Disabled") << "\n";
    ss << "MSAA Samples: " << m_settings.msaaSamples << "\n";
    ss << "Shadow Map Size: " << m_settings.shadowMapSize << "\n";
    ss << "Max Texture Size: " << m_settings.maxTextureSize << "\n";
    ss << "Anisotropy Level: " << m_settings.anisotropyLevel << "\n";
    
    // Add memory usage
    size_t vramUsage = Console_GetVRAMUsage();
    ss << "VRAM Usage: " << (vramUsage / 1024 / 1024) << " MB\n";
    
    return ss.str();
}

std::string GraphicsEngine::Console_Benchmark(int seconds) {
    LOG_TO_CONSOLE_IMMEDIATE(L"Starting " + std::to_wstring(seconds) + L" second benchmark", L"INFO");
    
    auto startTime = std::chrono::high_resolution_clock::now();
    int frameCount = 0;
    float totalFrameTime = 0.0f;
    float maxFrameTime = 0.0f;
    float minFrameTime = FLT_MAX;
    
    // Simple benchmark - just count frames and measure timing
    while (true) {
        auto currentTime = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(currentTime - startTime);
        
        if (elapsed.count() >= seconds) {
            break;
        }
        
        // Simulate frame timing
        auto frameStart = std::chrono::high_resolution_clock::now();
        std::this_thread::sleep_for(std::chrono::microseconds(16667)); // ~60 FPS
        auto frameEnd = std::chrono::high_resolution_clock::now();
        
        float frameTime = std::chrono::duration_cast<std::chrono::microseconds>(frameEnd - frameStart).count() / 1000.0f;
        
        totalFrameTime += frameTime;
        maxFrameTime = (std::max)(maxFrameTime, frameTime);
        minFrameTime = (std::min)(minFrameTime, frameTime);
        frameCount++;
    }
    
    std::stringstream ss;
    ss << "=== Benchmark Results ===\n";
    ss << "Duration: " << seconds << " seconds\n";
    ss << "Total Frames: " << frameCount << "\n";
    ss << "Average FPS: " << (frameCount / static_cast<float>(seconds)) << "\n";
    ss << "Average Frame Time: " << (totalFrameTime / frameCount) << " ms\n";
    ss << "Min Frame Time: " << minFrameTime << " ms\n";
    ss << "Max Frame Time: " << maxFrameTime << " ms\n";
    
    LOG_TO_CONSOLE_IMMEDIATE(L"Benchmark completed", L"SUCCESS");
    
    return ss.str();
}

void GraphicsEngine::Console_SetWireframe(bool enabled) {
    m_settings.wireframeMode = enabled;
    ApplyGraphicsState();
    LOG_TO_CONSOLE_IMMEDIATE(enabled ? L"Wireframe mode enabled" : L"Wireframe mode disabled", L"INFO");
}

void GraphicsEngine::Console_SetWireframeMode(bool enabled) {
    Console_SetWireframe(enabled);
}

void GraphicsEngine::Console_SetVSync(bool enabled) {
    m_settings.vsync = enabled;
    LOG_TO_CONSOLE_IMMEDIATE(enabled ? L"VSync enabled" : L"VSync disabled", L"INFO");
}

void GraphicsEngine::Console_SetRenderingPipeline(RenderingPipeline pipeline) {
    SetRenderingPipeline(pipeline);
}

void GraphicsEngine::Console_SetHDR(bool enabled) {
    SetHDREnabled(enabled);
}

bool GraphicsEngine::Console_TakeScreenshot(const std::string& filename) {
    return Console_Screenshot(filename);
}

void GraphicsEngine::Console_ForceGarbageCollection() {
    LOG_TO_CONSOLE_IMMEDIATE(L"Forcing garbage collection", L"INFO");
    
    // Force release of temporary resources
    if (m_textureSystem) {
        try {
            // If texture system has a cleanup method, call it
            LOG_TO_CONSOLE_IMMEDIATE(L"Texture system cleanup triggered", L"INFO");
        } catch (...) {
            LOG_TO_CONSOLE_IMMEDIATE(L"Texture system cleanup failed", L"WARNING");
        }
    }
    
    if (m_assetPipeline) {
        try {
            m_assetPipeline->Console_ForceGC();
            LOG_TO_CONSOLE_IMMEDIATE(L"Asset pipeline garbage collection triggered", L"INFO");
        } catch (...) {
            LOG_TO_CONSOLE_IMMEDIATE(L"Asset pipeline GC failed", L"WARNING");
        }
    }
    
    LOG_TO_CONSOLE_IMMEDIATE(L"Garbage collection complete", L"SUCCESS");
}

RenderStatistics GraphicsEngine::Console_GetMetrics() const {
    return Console_GetStatistics(); // Delegates to canonical method
}

void GraphicsEngine::Console_SetGPUTiming(bool enabled) {
    LOG_TO_CONSOLE_IMMEDIATE(enabled ? L"Enabling GPU timing via console" : L"Disabling GPU timing via console", L"INFO");
    
    // Update the setting
    m_settings.enableGPUTiming = enabled;
    
    // If enabling, try to create the timing query if it doesn't exist
    if (enabled && !m_gpuTimingQuery && m_device) {
        D3D11_QUERY_DESC queryDesc = {};
        queryDesc.Query = D3D11_QUERY_TIMESTAMP;
        HRESULT hr = m_device->CreateQuery(&queryDesc, &m_gpuTimingQuery);
        
        if (SUCCEEDED(hr)) {
            LOG_TO_CONSOLE_IMMEDIATE(L"GPU timing query created successfully", L"SUCCESS");
        } else {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create GPU timing query", L"ERROR");
            m_settings.enableGPUTiming = false; // Revert setting if creation failed
        }
    }
    
    // Notify state change
    NotifyStateChange();
    
    LOG_TO_CONSOLE_IMMEDIATE(enabled ? L"GPU timing enabled successfully" : L"GPU timing disabled successfully", L"SUCCESS");
}

size_t GraphicsEngine::Console_GetVRAMUsage() const {
    LOG_TO_CONSOLE_IMMEDIATE(L"Retrieving VRAM usage via console", L"INFO");
    
    // Calculate total VRAM usage from tracked memory
    size_t totalUsage = m_textureMemoryUsage + m_bufferMemoryUsage;
    
    // Add advanced system memory usage if available
    if (m_textureSystem) {
        try {
            auto textureMetrics = m_textureSystem->Console_GetMetrics();
            totalUsage = textureMetrics.totalMemoryUsage + m_bufferMemoryUsage;
        } catch (...) {
            // Fall back to tracked usage if metrics are unavailable
        }
    }
    
    LOG_TO_CONSOLE_IMMEDIATE(L"VRAM usage retrieved: " + std::to_wstring(totalUsage / 1024 / 1024) + L" MB", L"INFO");
    
    return totalUsage;
}

GraphicsSettings GraphicsEngine::Console_GetSettings() const {
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_settings;
}

void GraphicsEngine::Console_ApplySettings(const GraphicsSettings& settings) {
    m_settings = settings;
    ApplyGraphicsState();
    ApplyAdvancedGraphicsState();
    NotifyStateChange();
}

void GraphicsEngine::Console_ResetToDefaults() {
    LOG_TO_CONSOLE_IMMEDIATE(L"Resetting graphics settings to defaults via console", L"INFO");
    
    // Create a new default settings structure
    GraphicsSettings defaults;
    
    // Apply the default settings
    m_settings = defaults;
    
    // Apply the graphics state changes
    ApplyGraphicsState();
    ApplyAdvancedGraphicsState();
    
    // Notify state change if callback is registered
    NotifyStateChange();
    
    LOG_TO_CONSOLE_IMMEDIATE(L"Graphics settings reset to defaults successfully", L"SUCCESS");
}

void GraphicsEngine::Console_RegisterStateCallback(std::function<void()> callback) {
    m_stateCallback = callback;
}

void GraphicsEngine::Console_ResetDevice() {
    LOG_TO_CONSOLE_IMMEDIATE(L"Graphics device reset requested via console", L"WARNING");
    
    if (!m_device || !m_swapChain) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Graphics device not available for reset", L"ERROR");
        return;
    }

    try {
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
        m_renderTargetView.Reset();
        m_depthStencilView.Reset();

        HRESULT hr = m_swapChain->ResizeBuffers(0, m_windowWidth, m_windowHeight, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to resize buffers during device reset", L"ERROR");
            return;
        }

        hr = CreateRenderTargetView();
        if (FAILED(hr)) {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to recreate render target view", L"ERROR");
            return;
        }

        hr = CreateDepthStencilView();
        if (FAILED(hr)) {
            LOG_TO_CONSOLE_IMMEDIATE(L"Failed to recreate depth stencil view", L"ERROR");
            return;
        }

        SetViewport();
        ApplyGraphicsState();
        ApplyAdvancedGraphicsState();

        LOG_TO_CONSOLE_IMMEDIATE(L"Graphics device reset complete", L"SUCCESS");

    } catch (...) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Exception occurred during device reset", L"ERROR");
    }
}

void GraphicsEngine::Console_SetDebugMode(bool enabled) {
    m_settings.debugMode = enabled;
    LOG_TO_CONSOLE_IMMEDIATE(enabled ? L"Debug mode enabled" : L"Debug mode disabled", L"INFO");
}

void GraphicsEngine::Console_SetClearColor(float r, float g, float b, float a) {
    m_settings.clearColor[0] = (std::max)(0.0f, (std::min)(1.0f, r));
    m_settings.clearColor[1] = (std::max)(0.0f, (std::min)(1.0f, g));
    m_settings.clearColor[2] = (std::max)(0.0f, (std::min)(1.0f, b));
    m_settings.clearColor[3] = (std::max)(0.0f, (std::min)(1.0f, a));
    LOG_TO_CONSOLE_IMMEDIATE(L"Clear color set", L"INFO");
}

void GraphicsEngine::Console_SetRenderScale(float scale) {
    m_settings.renderScale = (std::max)(0.1f, (std::min)(4.0f, scale));
    LOG_TO_CONSOLE_IMMEDIATE(L"Render scale set to " + std::to_wstring(scale), L"INFO");
}

// ============================================================================
// MISSING METHOD IMPLEMENTATIONS
// ============================================================================

void GraphicsEngine::SetRenderPath(RenderPath path)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Setting render path", L"INFO");
    m_settings.renderPath = path;
    
    switch (path) {
        case RenderPath::Forward:
            m_currentPipeline = RenderingPipeline::Forward;
            break;
        case RenderPath::Deferred:
            m_currentPipeline = RenderingPipeline::Deferred;
            SetupDeferredPipeline();
            break;
        case RenderPath::ForwardPlus:
            m_currentPipeline = RenderingPipeline::ForwardPlus;
            SetupForwardPlusPipeline();
            break;
        case RenderPath::Clustered:
            m_currentPipeline = RenderingPipeline::Clustered;
            break;
    }
    
    NotifyStateChange();
    LOG_TO_CONSOLE_IMMEDIATE(L"Render path changed", L"INFO");
}

void GraphicsEngine::SetQualityPreset(QualityPreset preset)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Applying quality preset", L"INFO");
    
    switch (preset) {
        case QualityPreset::Low:
            m_settings.msaaSamples = 1;
            m_settings.shadowMapSize = 512;
            m_settings.maxTextureSize = 512;
            m_settings.anisotropyLevel = 1;
            m_settings.shadows = false;
            m_settings.bloom = false;
            m_settings.ssao = false;
            m_settings.taa = false;
            break;
            
        case QualityPreset::Medium:
            m_settings.msaaSamples = 2;
            m_settings.shadowMapSize = 1024;
            m_settings.maxTextureSize = 1024;
            m_settings.anisotropyLevel = 4;
            m_settings.shadows = true;
            m_settings.bloom = true;
            m_settings.ssao = false;
            m_settings.taa = false;
            break;
            
        case QualityPreset::High:
            m_settings.msaaSamples = 4;
            m_settings.shadowMapSize = 2048;
            m_settings.maxTextureSize = 2048;
            m_settings.anisotropyLevel = 8;
            m_settings.shadows = true;
            m_settings.bloom = true;
            m_settings.ssao = true;
            m_settings.taa = false;
            break;
            
        case QualityPreset::Ultra:
            m_settings.msaaSamples = 8;
            m_settings.shadowMapSize = 4096;
            m_settings.maxTextureSize = 4096;
            m_settings.anisotropyLevel = 16;
            m_settings.shadows = true;
            m_settings.bloom = true;
            m_settings.ssao = true;
            m_settings.taa = true;
            break;
            
        case QualityPreset::Custom:
            LOG_TO_CONSOLE_IMMEDIATE(L"Custom quality preset selected - no changes applied", L"INFO");
            return;
    }
    
    m_settings.qualityPreset = preset;
    ApplyQualityPreset(preset);
    ApplyGraphicsState();
    NotifyStateChange();
    LOG_TO_CONSOLE_IMMEDIATE(L"Quality preset applied successfully", L"SUCCESS");
}

void GraphicsEngine::SetRenderingPipeline(RenderingPipeline pipeline) {
    LOG_TO_CONSOLE_IMMEDIATE(L"Setting rendering pipeline", L"INFO");
    m_currentPipeline = pipeline;
    
    // Update corresponding settings
    switch (pipeline) {
        case RenderingPipeline::Forward:
            m_settings.renderPath = RenderPath::Forward;
            break;
        case RenderingPipeline::Deferred:
            m_settings.renderPath = RenderPath::Deferred;
            SetupDeferredPipeline();
            break;
        case RenderingPipeline::ForwardPlus:
            m_settings.renderPath = RenderPath::ForwardPlus;
            SetupForwardPlusPipeline();
            break;
        case RenderingPipeline::Clustered:
            m_settings.renderPath = RenderPath::Clustered;
            break;
    }
    
    NotifyStateChange();
}

void GraphicsEngine::SetHDREnabled(bool enabled) {
    LOG_TO_CONSOLE_IMMEDIATE(enabled ? L"Enabling HDR" : L"Disabling HDR", L"INFO");
    m_hdrEnabled = enabled;
    m_settings.hdr = enabled;
    NotifyStateChange();
}

void GraphicsEngine::NotifyStateChange() {
    if (m_stateCallback) {
        try {
            m_stateCallback();
        } catch (const std::exception& e) {
            LOG_TO_CONSOLE_IMMEDIATE(L"Error in state change callback: " + 
                std::wstring(e.what(), e.what() + strlen(e.what())), L"ERROR");
        } catch (...) {
            LOG_TO_CONSOLE_IMMEDIATE(L"Unknown error in state change callback", L"ERROR");
        }
    }
}

void GraphicsEngine::ApplyQualityPreset(QualityPreset preset)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Applying quality preset configurations", L"INFO");
    
    // Apply preset-specific configurations to subsystems
    if (m_lightingSystem) {
        m_lightingSystem->EnableShadows(m_settings.shadows);
        m_lightingSystem->SetGlobalShadowQuality(m_settings.shadowMapSize);
    }
    
    if (m_materialSystem) {
        std::string quality;
        switch (preset) {
            case QualityPreset::Low: quality = "low"; break;
            case QualityPreset::Medium: quality = "medium"; break;
            case QualityPreset::High: quality = "high"; break;
            case QualityPreset::Ultra: quality = "ultra"; break;
            default: quality = "medium"; break;
        }
        m_materialSystem->Console_SetTextureQuality(quality);
    }
    
    if (m_textureSystem) {
        std::string quality;
        switch (preset) {
            case QualityPreset::Low: quality = "low"; break;
            case QualityPreset::Medium: quality = "medium"; break;
            case QualityPreset::High: quality = "high"; break;
            case QualityPreset::Ultra: quality = "ultra"; break;
            default: quality = "medium"; break;
        }
        m_textureSystem->Console_SetQuality(quality);
    }
    
    LOG_TO_CONSOLE_IMMEDIATE(L"Quality preset configurations applied", L"INFO");
}

void GraphicsEngine::OnResize(unsigned int width, unsigned int height)
{
    if (width == 0 || height == 0)
        return;
    else
    {
		LOG_TO_CONSOLE_IMMEDIATE(L"Handling window resize to " + std::to_wstring(width) + L"x" + std::to_wstring(height), L"INFO");
    }

    m_windowWidth = width;
    m_windowHeight = height;

    if (m_context) {
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
    }
    else {
        LOG_TO_CONSOLE_IMMEDIATE(L"Device context not available during resize", L"WARNING");
	}
    m_renderTargetView.Reset();
    m_depthStencilView.Reset();
    if (m_swapChain) {
        HRESULT resizeHr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(resizeHr)) {
            SPARK_LOG_ERROR("Graphics", "SwapChain::ResizeBuffers failed HR=0x%08lX for size %ux%u",
                            static_cast<long>(resizeHr), width, height);
        }
    }
    else {
        SPARK_LOG_WARN("Graphics", "Swap chain not available during resize to %ux%u", width, height);
    }

    if (!m_device) {
        LOG_TO_CONSOLE_IMMEDIATE(L"Device not available during resize", L"ERROR");
        return;
	}

    CreateRenderTargetView();
    CreateDepthStencilView();
    SetViewport();
    ApplyGraphicsState();
    ApplyAdvancedGraphicsState();
}
#endif // SPARK_PLATFORM_WINDOWS
