/**
 * @file ShaderWindows.cpp
 * @brief Windows/D3D11 implementation — split from Shader.cpp
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

/**
 * @file Shader.cpp
 * @brief Core shader state: construction, initialization, and binding
 *
 * Manages the Shader object lifecycle (construction, initialization, shutdown),
 * pipeline binding/unbinding, and thread-safe metrics access. Constant buffer
 * creation and updates live in ShaderWindowsConstantBuffers.cpp; compilation
 * logic lives in ShaderCompilation.cpp; hot-reload and variant management live
 * in ShaderHotReload.cpp; console integration methods live in ShaderConsoleOps.cpp.
 */
#include "Shader.h"
#include "Utils/Assert.h"
#include "../Utils/Validate.h"
#include "../Utils/SparkConsole.h"
// Phase U: activated Tier 2 graphics orphan — process-wide singleton file
// watcher that recompiles shaders when their source .hlsl files change on
// disk. Wired into Shader::Initialize / Shader::LoadFromFile /
// Shader::HotReloadShaders so every Shader instance shares the same
// registry of watched directories.
#include "ShaderHotReload.h"
// Phase V: activated Tier 2 graphics orphan — persistent on-disk shader
// cache. Wired into Shader::Initialize so every Shader instance queries
// the same process-wide cache before calling the backend compiler.
#include "ShaderDiskCache.h"
// Phase W: activated Tier 2 graphics orphan — in-memory shader cross-
// compilation cache. Shared singleton reachable from any Shader call
// path so asset cookers and tests have a stable handle to the cross-
// compile surface.
#include "ShaderCrossCompiler.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <d3dcompiler.h>
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include "RHI/RHIFactory.h"
#include "RHI/RHITypes.h"
#include "Utils/LocalFileCache.h"
#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#endif // SPARK_PLATFORM_WINDOWS

#ifdef SPARK_PLATFORM_WINDOWS

using namespace DirectX;

// Console logging — use centralized macros from LogMacros.h
#include "../Utils/LogMacros.h"

// ============================================================================
// SHADER RESOURCE IMPLEMENTATIONS
// ============================================================================

void VertexShaderResource::Bind(ID3D11DeviceContext* context)
{
    if (m_vertexShader && m_inputLayout)
    {
        context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
        context->IASetInputLayout(m_inputLayout.Get());
    }
}

void VertexShaderResource::Unbind(ID3D11DeviceContext* context)
{
    context->VSSetShader(nullptr, nullptr, 0);
    context->IASetInputLayout(nullptr);
}

void PixelShaderResource::Bind(ID3D11DeviceContext* context)
{
    if (m_pixelShader)
    {
        context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
    }
}

void PixelShaderResource::Unbind(ID3D11DeviceContext* context)
{
    context->PSSetShader(nullptr, nullptr, 0);
}

// ============================================================================
// MAIN SHADER CLASS IMPLEMENTATION
// ============================================================================

Shader::Shader()
    : m_device(nullptr), m_context(nullptr), m_activeVariant(-1), m_hotReloadEnabled(true), m_validationEnabled(true)
{
    // Initialize default compilation flags for C++14 compatibility
    m_defaultFlags.enableDebug = false;
    m_defaultFlags.enableOptimization = true;
    m_defaultFlags.enableValidation = true;
    m_defaultFlags.treatWarningsAsErrors = false;
    m_defaultFlags.entryPoint = "main";

    // Initialize metrics for C++14 compatibility
    m_metrics.compiledShaders = 0;
    m_metrics.failedCompilations = 0;
    m_metrics.activeVariants = 0;
    m_metrics.hotReloadCount = 0;
    m_metrics.lastCompileTime = 0.0f;
    m_metrics.totalCompileTime = 0.0f;
    m_metrics.shaderMemoryUsage = 0;
    m_metrics.hotReloadEnabled = false;

    // Search paths for shader files — relative only, no hardcoded absolute paths
    m_searchPaths.clear();
    m_searchPaths.push_back("../SparkEngine/Shaders/HLSL/");
    m_searchPaths.push_back("../../SparkEngine/Shaders/HLSL/");
    m_searchPaths.push_back("Shaders/HLSL/");
    m_searchPaths.push_back("Assets/Shaders/");
    m_searchPaths.push_back("./");

    LOG_TO_CONSOLE_IMMEDIATE(L"Enhanced Shader system constructed with AAA features and console integration.", L"INFO");
}

Shader::~Shader()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Enhanced Shader destructor called.", L"INFO");
    Shutdown();
}

HRESULT Shader::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, device);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, context);

    m_device = device;
    m_context = context;

    HRESULT hr = CreateConstantBuffers();
    if (FAILED(hr))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Shader constant buffer creation failed with HR=0x%08lX",
                        static_cast<long>(hr));
        return hr;
    }

    m_vertexShader = std::make_unique<VertexShaderResource>();
    m_pixelShader = std::make_unique<PixelShaderResource>();

    // Phase O: activate the shader variant system alongside the existing
    // shader resources. Zero dependencies on the D3D11 device — pure
    // CPU keyword / variant bookkeeping.
    m_variantSystem.Initialize();

    // Phase U: activate Spark::Graphics::ShaderHotReload singleton. The
    // first Shader::Initialize call initialises the process-wide file
    // watcher with each search path that exists on disk; subsequent
    // Shader instances and LoadFromFile calls add further watch
    // directories on demand via AddWatchDirectory.
    auto& hotReload = Spark::Graphics::ShaderHotReload::GetInstance();
    if (!hotReload.IsWatching())
    {
        std::error_code ec;
        bool initialized = false;
        for (const auto& path : m_searchPaths)
        {
            if (std::filesystem::exists(path, ec))
            {
                hotReload.Initialize(path);
                initialized = true;
                break;
            }
        }
        if (!initialized)
        {
            hotReload.Initialize(".");
        }
    }
    else
    {
        for (const auto& path : m_searchPaths)
        {
            std::error_code ec;
            if (std::filesystem::exists(path, ec))
            {
                hotReload.AddWatchDirectory(path);
            }
        }
    }

    // Phase V: activate Spark::Graphics::ShaderDiskCache singleton. The
    // first Shader::Initialize call creates the cache directory under
    // the working directory ("ShaderCache/"); subsequent Shader
    // instances reuse the same cache. Tests may override the directory
    // via GetShaderDiskCache().Initialize(path) before the first
    // Shader::Initialize runs.
    auto& diskCache = Spark::Graphics::GetShaderDiskCache();
    if (!diskCache.IsInitialized())
    {
        diskCache.Initialize(std::filesystem::path("ShaderCache"));
    }

    // Phase W: activate Spark::Graphics::ShaderCrossCompiler singleton.
    // In-memory compile cache used by CompileAll / CompileAsync. The
    // internal per-target Compile* functions are currently stubs that
    // report success without producing bytecode — Phase W wires the
    // lifecycle so tests and future asset cookers that need a
    // cross-target compile surface have a shared instance to talk to,
    // and so any real DXC / SPIRV-Cross integration slots into this
    // existing activation.
    auto& crossCompiler = Spark::Graphics::GetShaderCrossCompiler();
    if (!crossCompiler.IsInitialized())
    {
        crossCompiler.Initialize();
    }

    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Shader system initialized");
    return S_OK;
}

void Shader::Shutdown()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Shader system shutting down (%zu cached, %zu variants)",
                   m_shaderCache.size(), m_shaderVariants.size());

    // Clear shader cache
    m_shaderCache.clear();
    m_shaderVariants.clear();

    // Reset DirectX resources
    m_postProcessingBuffer.Reset();
    m_lightingDataBuffer.Reset();
    m_perMaterialBuffer.Reset();
    m_perObjectBuffer.Reset();
    m_perFrameBuffer.Reset();

    m_pixelShader.reset();
    m_vertexShader.reset();

    // Phase O: tear down the variant system — clears all keyword
    // registrations and groups so a subsequent Initialize starts clean.
    m_variantSystem.Shutdown();

    m_device = nullptr;
    m_context = nullptr;

    LOG_TO_CONSOLE_IMMEDIATE(L"Enhanced Shader shutdown complete.", L"INFO");
}

// ============================================================================
// SHADER BINDING AND STATE
// ============================================================================

void Shader::SetShaders()
{
    ASSERT(m_context != nullptr);

    if (m_vertexShader && m_vertexShader->IsValid())
    {
        m_vertexShader->Bind(m_context);
    }

    if (m_pixelShader && m_pixelShader->IsValid())
    {
        m_pixelShader->Bind(m_context);
    }

    // Bind constant buffers to appropriate slots
    ID3D11Buffer* buffers[] = {m_perFrameBuffer.Get(), m_perObjectBuffer.Get(), m_perMaterialBuffer.Get(),
                               m_lightingDataBuffer.Get(), m_postProcessingBuffer.Get()};

    m_context->VSSetConstantBuffers(0, 5, buffers);
    m_context->PSSetConstantBuffers(0, 5, buffers);
}

void Shader::UnbindShaders()
{
    ASSERT(m_context != nullptr);

    if (m_vertexShader)
    {
        m_vertexShader->Unbind(m_context);
    }

    if (m_pixelShader)
    {
        m_pixelShader->Unbind(m_context);
    }
}

bool Shader::IsValid() const
{
    return m_vertexShader && m_vertexShader->IsValid() && m_pixelShader && m_pixelShader->IsValid();
}

// Constant buffer management (UpdatePerFrameConstants, UpdatePerObjectConstants,
// UpdatePerMaterialConstants, UpdateLightingData, UpdatePostProcessingConstants,
// UpdateConstantBuffer, CreateConstantBuffers) lives in
// ShaderWindowsConstantBuffers.cpp.

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

void Shader::NotifyStateChange()
{
    // Notify any registered callbacks about shader state changes
    LOG_TO_CONSOLE_IMMEDIATE(L"Shader state changed", L"DEBUG");
}

Shader::ShaderMetrics Shader::GetMetricsThreadSafe() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_metrics;
}

#endif // inner SPARK_PLATFORM_WINDOWS


#endif // SPARK_PLATFORM_WINDOWS
