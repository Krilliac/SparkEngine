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
#include "GraphicsEngineRHI.h"
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

    // Acquire the RHI device for shader/pipeline creation on the OpenGL path.
    auto& rhiState = Spark::Graphics::Detail::GetRHI();
    m_rhiDevice = rhiState.initialized ? rhiState.bridge.GetDevice() : nullptr;

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
    // Release RHI resources before the device goes away
    m_rhiPipeline.reset();
    m_rhiVertexShader.reset();
    m_rhiPixelShader.reset();
    m_rhiPerFrameCB.reset();
    m_rhiPerObjectCB.reset();
    m_rhiDevice = nullptr;
    m_compiledVertexSource.clear();
    m_compiledPixelSource.clear();

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
    // Create RHI constant buffers for the OpenGL path. These are bound to
    // UBO binding points 0 (per-frame) and 1 (per-object) matching the
    // GLSL layout(std140, binding=N) declarations.
    if (m_rhiDevice)
    {
        Spark::RHI::RHIBufferDesc cbDesc;
        cbDesc.usage = Spark::RHI::RHIBufferUsage::Constant;
        cbDesc.access = Spark::RHI::RHIBufferAccess::Dynamic;

        cbDesc.size = sizeof(PerFrameConstants);
        cbDesc.debugName = "PerFrameCB";
        m_rhiPerFrameCB = m_rhiDevice->CreateBuffer(cbDesc);

        cbDesc.size = sizeof(PerObjectConstants);
        cbDesc.debugName = "PerObjectCB";
        m_rhiPerObjectCB = m_rhiDevice->CreateBuffer(cbDesc);
    }

    return S_OK;
}

void Shader::CreateRHIPipelineIfReady()
{
    // Already built
    if (m_rhiPipeline)
        return;

    // Need both shaders and a device
    if (!m_rhiDevice || m_compiledVertexSource.empty() || m_compiledPixelSource.empty())
        return;

    // Create vertex shader
    Spark::RHI::RHIShaderDesc vsDesc;
    vsDesc.stage = Spark::RHI::RHIShaderStage::Vertex;
    vsDesc.sourceCode = m_compiledVertexSource;
    vsDesc.entryPoint = "main";
    vsDesc.language = Spark::RHI::ShaderLanguage::GLSL;
    vsDesc.debugName = m_filePath + "_VS";
    m_rhiVertexShader = m_rhiDevice->CreateShader(vsDesc);

    // Create pixel shader
    Spark::RHI::RHIShaderDesc psDesc;
    psDesc.stage = Spark::RHI::RHIShaderStage::Pixel;
    psDesc.sourceCode = m_compiledPixelSource;
    psDesc.entryPoint = "main";
    psDesc.language = Spark::RHI::ShaderLanguage::GLSL;
    psDesc.debugName = m_filePath + "_PS";
    m_rhiPixelShader = m_rhiDevice->CreateShader(psDesc);

    if (!m_rhiVertexShader || !m_rhiPixelShader)
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics, "Failed to create RHI shaders from compiled GLSL source");
        return;
    }

    // Create pipeline state
    Spark::RHI::RHIPipelineStateDesc psoDesc;
    psoDesc.debugName = m_filePath + "_PSO";
    m_rhiPipeline = m_rhiDevice->CreatePipelineState(psoDesc, m_rhiVertexShader.get(), m_rhiPixelShader.get());

    if (m_rhiPipeline)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "RHI pipeline state created for shader '%s'", m_filePath.c_str());
    }
}

// ============================================================================
// SHADER BINDING (no-ops on Linux - uses RHI pipeline instead)
// ============================================================================

void Shader::SetShaders()
{
    // Bind the RHI pipeline state on the OpenGL path. The pipeline is
    // created lazily the first time both VS and PS are compiled.
    CreateRHIPipelineIfReady();

    auto& rhiState = Spark::Graphics::Detail::GetRHI();
    if (!rhiState.initialized)
        return;

    auto* cmd = rhiState.bridge.GetCommandList();
    if (!cmd)
        return;

    if (m_rhiPipeline)
        cmd->SetPipelineState(m_rhiPipeline.get());

    // Bind constant buffers to the expected UBO slots
    if (m_rhiPerFrameCB)
        cmd->SetConstantBuffer(Spark::RHI::RHIShaderStage::Vertex, 0, m_rhiPerFrameCB.get());
    if (m_rhiPerObjectCB)
        cmd->SetConstantBuffer(Spark::RHI::RHIShaderStage::Vertex, 1, m_rhiPerObjectCB.get());
}

void Shader::UnbindShaders()
{
    // On OpenGL the pipeline state persists until the next SetPipelineState call.
    // No explicit unbind needed.
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
    if (m_rhiDevice && m_rhiPerFrameCB)
        m_rhiDevice->UpdateBuffer(m_rhiPerFrameCB.get(), &constants, sizeof(constants), 0);
}

void Shader::UpdatePerObjectConstants(const PerObjectConstants& constants)
{
    if (m_rhiDevice && m_rhiPerObjectCB)
        m_rhiDevice->UpdateBuffer(m_rhiPerObjectCB.get(), &constants, sizeof(constants), 0);
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
