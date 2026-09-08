/**
 * @file ShaderCompilationWindowsFileLoad.cpp
 * @brief Windows D3D11 vertex and pixel shader file-loading implementation
 */

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS

#include "Shader.h"
#include "ShaderHotReload.h"
#include "Utils/Assert.h"

#include "../Utils/LogMacros.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/Validate.h"

#include <d3dcompiler.h>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <windows.h>

using namespace DirectX;

// ============================================================================
// SHADER LOADING — VERTEX AND PIXEL
// ============================================================================

HRESULT Shader::LoadVertexShader(const std::wstring& filename, const ShaderCompilationFlags& flags)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, !filename.empty(), "LoadVertexShader: filename empty");
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, m_device);

    SPARK_LOG_INFO(Spark::LogCategory::Graphics, "Compiling vertex shader: %ls", filename.c_str());

    auto startTime = std::chrono::high_resolution_clock::now();

    ComPtr<ID3DBlob> vsBlob;

    // **FIX: Explicitly specify vertex shader compilation**
    ShaderCompilationFlags vsFlags = flags;
    vsFlags.target = "vs_5_0"; // Force vertex shader target

    HRESULT hr = CompileShaderFromFileAdvanced(filename, ShaderType::VERTEX_SHADER, vsFlags, &vsBlob);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Vertex shader compilation failed, trying fallback method", L"WARNING");

        // **FALLBACK: Try direct D3DCompileFromFile**
        ComPtr<ID3DBlob> errorBlob;
        hr = D3DCompileFromFile(filename.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "vs_5_0",
                                D3DCOMPILE_ENABLE_STRICTNESS, 0, &vsBlob, &errorBlob);

        if (FAILED(hr))
        {
            if (errorBlob)
            {
                std::string errorString(reinterpret_cast<const char*>(errorBlob->GetBufferPointer()));
                std::wstring wErrorString(errorString.begin(), errorString.end());
                LOG_TO_CONSOLE_IMMEDIATE(L"Vertex shader compilation error: " + wErrorString, L"ERROR");
            }
            return hr;
        }
    }

    m_vertexShader = std::make_unique<VertexShaderResource>();
    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                      m_vertexShader->m_vertexShader.GetAddressOf());
    if (FAILED(hr))
    {
        std::wstring errorMsg = L"CreateVertexShader failed with HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    m_vertexShader->m_shaderBlob = vsBlob;

    hr = CreateInputLayout(vsBlob.Get(), m_vertexShader->m_inputLayout.GetAddressOf());
    if (SUCCEEDED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Vertex shader and input layout loaded successfully", L"SUCCESS");
    }
    else
    {
        std::wstring errorMsg = L"CreateInputLayout failed with HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
    }

    // Phase U: register the parent directory with the ShaderHotReload
    // singleton so runtime file-watching picks up this file.
    {
        std::string narrowFile(filename.begin(), filename.end());
        std::error_code ec;
        std::filesystem::path parent = std::filesystem::path(narrowFile).parent_path();
        if (!parent.empty() && std::filesystem::exists(parent, ec))
        {
            Spark::Graphics::ShaderHotReload::GetInstance().AddWatchDirectory(parent.string());
        }
    }

    return hr;
}

HRESULT Shader::LoadPixelShader(const std::wstring& filename, const ShaderCompilationFlags& flags)
{
    LOG_TO_CONSOLE_IMMEDIATE(std::wstring(L"Loading enhanced pixel shader: ") + filename, L"INFO");

    auto startTime = std::chrono::high_resolution_clock::now();

    ASSERT_MSG(!filename.empty(), "LoadPixelShader: filename empty");
    ASSERT_MSG(m_device != nullptr, "LoadPixelShader: device is null");

    ComPtr<ID3DBlob> psBlob;

    // **FIX: Explicitly specify pixel shader compilation**
    ShaderCompilationFlags psFlags = flags;
    psFlags.target = "ps_5_0"; // Force pixel shader target

    HRESULT hr = CompileShaderFromFileAdvanced(filename, ShaderType::PIXEL_SHADER, psFlags, &psBlob);
    if (FAILED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Pixel shader compilation failed, trying fallback method", L"WARNING");

        // **FALLBACK: Try direct D3DCompileFromFile**
        ComPtr<ID3DBlob> errorBlob;
        hr = D3DCompileFromFile(filename.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "ps_5_0",
                                D3DCOMPILE_ENABLE_STRICTNESS, 0, &psBlob, &errorBlob);

        if (FAILED(hr))
        {
            if (errorBlob)
            {
                std::string errorString(reinterpret_cast<const char*>(errorBlob->GetBufferPointer()));
                std::wstring wErrorString(errorString.begin(), errorString.end());
                LOG_TO_CONSOLE_IMMEDIATE(L"Pixel shader compilation error: " + wErrorString, L"ERROR");
            }
            return hr;
        }
    }

    m_pixelShader = std::make_unique<PixelShaderResource>();
    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
                                     m_pixelShader->m_pixelShader.GetAddressOf());

    if (SUCCEEDED(hr))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Pixel shader loaded successfully", L"SUCCESS");
    }
    else
    {
        std::wstring errorMsg = L"CreatePixelShader failed with HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
    }

    // Phase U: register the parent directory with the ShaderHotReload
    // singleton so runtime file-watching picks up this file.
    {
        std::string narrowFile(filename.begin(), filename.end());
        std::error_code ec;
        std::filesystem::path parent = std::filesystem::path(narrowFile).parent_path();
        if (!parent.empty() && std::filesystem::exists(parent, ec))
        {
            Spark::Graphics::ShaderHotReload::GetInstance().AddWatchDirectory(parent.string());
        }
    }

    return hr;
}

#endif // SPARK_PLATFORM_WINDOWS
