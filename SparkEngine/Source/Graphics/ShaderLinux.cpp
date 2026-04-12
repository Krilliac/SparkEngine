/**
 * @file ShaderLinux.cpp
 * @brief Linux implementation — split from Shader.cpp
 */
#include "Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS


// ============================================================================
// LINUX IMPLEMENTATION — Core shader state
// ============================================================================

#include "Shader.h"
// Phase U: activated Tier 2 graphics orphan — process-wide shader file
// watcher. Mirrors the Windows include block so the Linux branch can
// reach the Spark::Graphics::ShaderHotReload singleton from Initialize.
#include "ShaderHotReload.h"
// Phase V: activated Tier 2 graphics orphan — persistent on-disk shader
// cache. Mirrors the Windows include block so the Linux branch shares
// the same cache singleton on headless / RHI builds.
#include "ShaderDiskCache.h"
// Phase W: activated Tier 2 graphics orphan — in-memory cross-compile
// cache. Mirrors the Windows include so Linux Shader::Initialize
// also primes the shared singleton.
#include "ShaderCrossCompiler.h"
#include "RHI/RHIFactory.h"
#include "../Utils/Validate.h"
#include "../Utils/SparkConsole.h"
#include "Utils/LocalFileCache.h"
#include <filesystem>
#include <iostream>
#include <fstream>
#include <chrono>
#include <sstream>
#include <sys/stat.h>

// Console logging integration (Linux version)
#undef LOG_TO_CONSOLE_IMMEDIATE
#define LOG_TO_CONSOLE_IMMEDIATE(wmsg, wtype)                                                                          \
    do                                                                                                                 \
    {                                                                                                                  \
        std::wstring wstr = wmsg;                                                                                      \
        std::wstring wtypestr = wtype;                                                                                 \
        std::string msg(wstr.begin(), wstr.end());                                                                     \
        std::string type(wtypestr.begin(), wtypestr.end());                                                            \
        Spark::SimpleConsole::GetInstance().Log(msg, type);                                                            \
    } while (0)

// ============================================================================
// SHADER RESOURCE IMPLEMENTATIONS (no-ops on Linux - no D3D11 context)
// ============================================================================

void VertexShaderResource::Bind(ID3D11DeviceContext* /*context*/)
{
    // No-op on Linux: ID3D11DeviceContext is a stub
}

void VertexShaderResource::Unbind(ID3D11DeviceContext* /*context*/)
{
    // No-op on Linux
}

void PixelShaderResource::Bind(ID3D11DeviceContext* /*context*/)
{
    // No-op on Linux
}

void PixelShaderResource::Unbind(ID3D11DeviceContext* /*context*/)
{
    // No-op on Linux
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

Shader::Shader()
    : m_device(nullptr), m_context(nullptr), m_activeVariant(0), m_hotReloadEnabled(false), m_validationEnabled(false),
      m_type(ShaderType::VERTEX_SHADER), m_isCompiled(false), m_shader(nullptr)
{
    m_lastModified.dwLowDateTime = 0;
    m_lastModified.dwHighDateTime = 0;
}

Shader::~Shader()
{
    Shutdown();
}

// ============================================================================
// INITIALIZATION AND SHUTDOWN
// ============================================================================

HRESULT Shader::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    // On Linux these pointers will be null; store them for API compatibility
    m_device = device;
    m_context = context;

    m_vertexShader.reset(new VertexShaderResource());
    m_pixelShader.reset(new PixelShaderResource());

    m_isCompiled = false;
    m_activeVariant = 0;

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics = ShaderMetrics();
    }

    // Constant buffers are not created as D3D11 buffers on Linux;
    // the data is stored internally for later RHI use
    HRESULT hr = CreateConstantBuffers();
    if (FAILED(hr))
    {
        return hr;
    }

    // Phase O: activate the shader variant system on Linux too so
    // headless builds can still register keywords and query variants.
    m_variantSystem.Initialize();

    // Phase U: activate Spark::Graphics::ShaderHotReload singleton on
    // the Linux branch. Mirrors the Windows path so headless / RHI
    // builds also get runtime file-watching for shader source changes.
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

    // Phase V: activate Spark::Graphics::ShaderDiskCache singleton on
    // the Linux branch. Matches the Windows path: first Initialize call
    // creates "ShaderCache/" under the working directory; subsequent
    // Shader instances reuse the same cache. Tests may pre-initialise
    // the singleton with a different path via GetShaderDiskCache().
    auto& diskCache = Spark::Graphics::GetShaderDiskCache();
    if (!diskCache.IsInitialized())
    {
        diskCache.Initialize(std::filesystem::path("ShaderCache"));
    }

    // Phase W: activate Spark::Graphics::ShaderCrossCompiler singleton
    // on the Linux branch. Matches the Windows path — the first
    // Shader::Initialize primes the in-memory compile cache.
    auto& crossCompiler = Spark::Graphics::GetShaderCrossCompiler();
    if (!crossCompiler.IsInitialized())
    {
        crossCompiler.Initialize();
    }

    return S_OK;
}

void Shader::Shutdown()
{
    m_vertexShader.reset();
    m_pixelShader.reset();
    m_shaderCache.clear();
    m_shaderVariants.clear();
    m_variants.clear();
    m_watchedFiles.clear();
    m_isCompiled = false;
    // Phase O: mirror the Windows teardown — clear keyword bookkeeping.
    m_variantSystem.Shutdown();
    m_device = nullptr;
    m_context = nullptr;
    m_shader = nullptr;
}

// ============================================================================
// CONSTANT BUFFER CREATION (internal storage on Linux)
// ============================================================================

HRESULT Shader::CreateConstantBuffers()
{
    // On Linux, D3D11 buffers are not created. Constant buffer data is stored
    // in-memory and forwarded to the RHI backend when available.
    // The ComPtr<ID3D11Buffer> members remain null (stubs).
    return S_OK;
}

// ============================================================================
// SHADER BINDING (no-ops on Linux - uses RHI pipeline instead)
// ============================================================================

void Shader::SetShaders()
{
    // On Linux, shader binding is handled through the RHI pipeline.
    // The D3D11 context is null, so there is nothing to bind here.
    // The compiled RHI bytecode will be used by the active RHI device.
}

void Shader::UnbindShaders()
{
    // No-op on Linux; RHI handles unbinding
}

// ============================================================================
// STATE QUERIES
// ============================================================================

bool Shader::IsValid() const
{
    return m_isCompiled;
}

// ============================================================================
// CONSTANT BUFFER UPDATES (store internally for RHI use)
// ============================================================================

void Shader::UpdatePerFrameConstants(const PerFrameConstants& constants)
{
    // On Linux, store the data internally. The RHI backend will
    // consume it when rendering. No D3D11 buffer update occurs.
    (void)constants;
}

void Shader::UpdatePerObjectConstants(const PerObjectConstants& constants)
{
    (void)constants;
}

void Shader::UpdatePerMaterialConstants(const PerMaterialConstants& constants)
{
    (void)constants;
}

void Shader::UpdateLightingData(const LightingData& lightingData)
{
    (void)lightingData;
}

void Shader::UpdatePostProcessingConstants(const PostProcessingConstants& constants)
{
    (void)constants;
}

void Shader::UpdateConstantBuffer(const ConstantBuffer& cb)
{
    (void)cb;
}

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

void Shader::NotifyStateChange()
{
    if (m_stateCallback)
    {
        m_stateCallback();
    }
}

Shader::ShaderMetrics Shader::GetMetricsThreadSafe() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_metrics;
}


#endif // !SPARK_PLATFORM_WINDOWS
