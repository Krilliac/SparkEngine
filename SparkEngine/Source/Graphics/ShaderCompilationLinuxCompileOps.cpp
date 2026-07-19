/**
 * @file ShaderCompilationLinuxCompileOps.cpp
 * @brief Linux compile utilities — split from ShaderCompilationLinux.cpp
 */
#include "Core/Platform.h"
#ifndef SPARK_PLATFORM_WINDOWS


// ============================================================================
// LINUX IMPLEMENTATION — Shader compile utilities via RHI pipeline
// ============================================================================

#include "Shader.h"
#include "RHI/RHIFactory.h"
#include "ShaderCompilationLinuxInternal.h"
#include <chrono>
#include <string>

// ============================================================================
// STATIC COMPILATION UTILITY (LEGACY — Linux)
// ============================================================================

HRESULT Shader::CompileShaderFromFile(const std::wstring& filename, const std::string& entryPoint,
                                      const std::string& shaderModel, ID3DBlob** blobOut)
{
    // On Linux, D3DCompileFromFile is not available. Use RHI compilation instead.
    std::string narrowPath = WideToNarrow(filename);

    if (!FileExistsLinux(narrowPath))
    {
        return E_FAIL;
    }

    std::string sourceCode;
    if (!ReadFileContents(narrowPath, sourceCode))
    {
        return E_FAIL;
    }

    // Determine shader stage from shader model string
    Spark::RHI::RHIShaderStage stage = Spark::RHI::RHIShaderStage::Vertex;
    if (shaderModel.find("ps_") == 0)
    {
        stage = Spark::RHI::RHIShaderStage::Pixel;
    }
    else if (shaderModel.find("gs_") == 0)
    {
        stage = Spark::RHI::RHIShaderStage::Geometry;
    }
    else if (shaderModel.find("hs_") == 0)
    {
        stage = Spark::RHI::RHIShaderStage::Hull;
    }
    else if (shaderModel.find("ds_") == 0)
    {
        stage = Spark::RHI::RHIShaderStage::Domain;
    }
    else if (shaderModel.find("cs_") == 0)
    {
        stage = Spark::RHI::RHIShaderStage::Compute;
    }

    Spark::RHI::ShaderCompileOptions options;
    options.stage = stage;
    options.sourceFile = narrowPath;
    options.sourceCode = sourceCode;
    options.entryPoint = entryPoint;
    options.sourceLanguage = Spark::RHI::ShaderLanguage::Auto;
    options.targetLanguage = Spark::RHI::ShaderLanguage::Auto;
    options.targetBackend = Spark::RHI::GraphicsBackend::Auto;

    Spark::RHI::ShaderCompileResult result = Spark::RHI::CompileShader(options);

    if (!result.success)
    {
        return E_FAIL;
    }

    // blobOut remains null on Linux (ID3DBlob is a stub type)
    if (blobOut)
    {
        *blobOut = nullptr;
    }

    return S_OK;
}

// ============================================================================
// PRIVATE HELPER METHODS (Linux)
// ============================================================================

HRESULT Shader::CompileShaderFromFileAdvanced(const std::wstring& filename, ShaderType type,
                                              const ShaderCompilationFlags& flags, ID3DBlob** blobOut)
{
    // Delegate to the RHI pipeline on Linux
    std::string narrowPath = WideToNarrow(filename);

    std::string sourceCode;
    if (!FileExistsLinux(narrowPath) || !ReadFileContents(narrowPath, sourceCode))
    {
        return E_FAIL;
    }

    Spark::RHI::ShaderCompileOptions options;
    options.stage = ShaderTypeToRHIStage(type);
    options.sourceFile = narrowPath;
    options.sourceCode = sourceCode;
    options.entryPoint = flags.entryPoint.empty() ? "main" : flags.entryPoint;
    options.optimizationEnabled = flags.enableOptimization;
    options.debugInfoEnabled = flags.enableDebug;
    options.defines = flags.defines;
    options.includePaths = flags.includePaths;
    options.sourceLanguage = Spark::RHI::ShaderLanguage::Auto;
    options.targetLanguage = Spark::RHI::ShaderLanguage::Auto;
    options.targetBackend = Spark::RHI::GraphicsBackend::Auto;

    Spark::RHI::ShaderCompileResult result = Spark::RHI::CompileShader(options);

    if (blobOut)
    {
        *blobOut = nullptr; // ID3DBlob is a stub on Linux
    }

    return result.success ? S_OK : E_FAIL;
}

HRESULT Shader::CreateInputLayout(ID3DBlob* /*vertexShaderBlob*/, ID3D11InputLayout** inputLayout)
{
    // No-op on Linux: input layout is a D3D11 concept.
    // The RHI backend handles vertex input configuration.
    if (inputLayout)
    {
        *inputLayout = nullptr;
    }
    return S_OK;
}

// ============================================================================
// RHI CROSS-PLATFORM COMPILATION (Linux)
// ============================================================================

bool Shader::CompileWithRHI(const std::string& sourceFile, ShaderType type, int targetBackend)
{
    auto startTime = std::chrono::high_resolution_clock::now();

    Spark::RHI::ShaderCompileOptions options;
    options.stage = ShaderTypeToRHIStage(type);
    options.sourceFile = sourceFile;
    options.entryPoint = m_defaultFlags.entryPoint;
    options.optimizationEnabled = m_defaultFlags.enableOptimization;
    options.debugInfoEnabled = m_defaultFlags.enableDebug;
    options.defines = m_defaultFlags.defines;
    options.includePaths = m_defaultFlags.includePaths;
    options.targetBackend = static_cast<Spark::RHI::GraphicsBackend>(targetBackend);

    // Read source if file exists
    std::string sourceCode;
    if (FileExistsLinux(sourceFile) && ReadFileContents(sourceFile, sourceCode))
    {
        options.sourceCode = sourceCode;
    }

    // Auto-detect source language from file extension
    options.sourceLanguage = Spark::RHI::ShaderLanguage::Auto;
    options.targetLanguage = Spark::RHI::ShaderLanguage::Auto;

    Spark::RHI::ShaderCompileResult result = Spark::RHI::CompileShader(options);

    auto endTime = std::chrono::high_resolution_clock::now();
    float compileTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.lastCompileTime = compileTimeMs;
        m_metrics.totalCompileTime += compileTimeMs;
    }

    if (!result.success)
    {
        std::wstring wError(result.errorMessage.begin(), result.errorMessage.end());
        LOG_TO_CONSOLE_IMMEDIATE(L"RHI shader compilation failed: " + wError, L"ERROR");
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.failedCompilations++;
        return false;
    }

    // Save compiled bytecode for later loading
    std::string outputPath = sourceFile;
    size_t dotPos = outputPath.find_last_of('.');
    if (dotPos != std::string::npos)
    {
        outputPath = outputPath.substr(0, dotPos);
    }

    // Determine output extension based on target
    Spark::RHI::GraphicsBackend backend = static_cast<Spark::RHI::GraphicsBackend>(targetBackend);
    if (backend == Spark::RHI::GraphicsBackend::Vulkan)
    {
        outputPath += ".spv";
    }
    else
    {
        outputPath += ".compiled";
    }

    Spark::RHI::SaveCompiledShader(outputPath, result.bytecode);

    m_isCompiled = true;

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.compiledShaders++;
        m_metrics.shaderMemoryUsage += result.bytecode.size();
    }

    std::wstring wFile(sourceFile.begin(), sourceFile.end());
    LOG_TO_CONSOLE_IMMEDIATE(L"RHI shader compiled successfully: " + wFile, L"SUCCESS");
    return true;
}


#endif // !SPARK_PLATFORM_WINDOWS
