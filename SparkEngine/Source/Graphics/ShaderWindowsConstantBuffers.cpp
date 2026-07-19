/**
 * @file ShaderWindowsConstantBuffers.cpp
 * @brief Windows/D3D11 constant buffer management — split from ShaderWindows.cpp
 *
 * Constant buffer creation and per-frame/per-object/per-material/lighting/
 * post-processing constant updates for the Shader class.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "Shader.h"
#include "Utils/Assert.h"
#include "../Utils/SparkConsole.h"
#include <string>

using namespace DirectX;

// Console logging — use centralized macros from LogMacros.h
#include "../Utils/LogMacros.h"

// ============================================================================
// CONSTANT BUFFER MANAGEMENT
// ============================================================================

void Shader::UpdatePerFrameConstants(const PerFrameConstants& constants)
{
    ASSERT(m_context != nullptr);
    ASSERT(m_perFrameBuffer != nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(m_perFrameBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr) && mapped.pData)
    {
        PerFrameConstants* data = reinterpret_cast<PerFrameConstants*>(mapped.pData);
        *data = constants;

        // Transpose matrices for HLSL
        data->ViewMatrix = XMMatrixTranspose(constants.ViewMatrix);
        data->ProjectionMatrix = XMMatrixTranspose(constants.ProjectionMatrix);
        data->ViewProjectionMatrix = XMMatrixTranspose(constants.ViewProjectionMatrix);

        m_context->Unmap(m_perFrameBuffer.Get(), 0);
    }
}

void Shader::UpdatePerObjectConstants(const PerObjectConstants& constants)
{
    ASSERT(m_context != nullptr);
    ASSERT(m_perObjectBuffer != nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(m_perObjectBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr) && mapped.pData)
    {
        PerObjectConstants* data = reinterpret_cast<PerObjectConstants*>(mapped.pData);
        *data = constants;

        // Transpose matrices for HLSL
        data->WorldMatrix = XMMatrixTranspose(constants.WorldMatrix);
        data->WorldViewProjectionMatrix = XMMatrixTranspose(constants.WorldViewProjectionMatrix);
        data->WorldInverseTransposeMatrix = XMMatrixTranspose(constants.WorldInverseTransposeMatrix);
        data->PreviousWorldMatrix = XMMatrixTranspose(constants.PreviousWorldMatrix);

        m_context->Unmap(m_perObjectBuffer.Get(), 0);
    }
}

void Shader::UpdatePerMaterialConstants(const PerMaterialConstants& constants)
{
    ASSERT(m_context != nullptr);
    ASSERT(m_perMaterialBuffer != nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(m_perMaterialBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr) && mapped.pData)
    {
        auto dataPtr = reinterpret_cast<PerMaterialConstants*>(mapped.pData);
        *dataPtr = constants;
        m_context->Unmap(m_perMaterialBuffer.Get(), 0);
    }
}

void Shader::UpdateLightingData(const LightingData& lightingData)
{
    ASSERT(m_context != nullptr);
    ASSERT(m_lightingDataBuffer != nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(m_lightingDataBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr) && mapped.pData)
    {
        auto dataPtr = reinterpret_cast<LightingData*>(mapped.pData);
        *dataPtr = lightingData;
        m_context->Unmap(m_lightingDataBuffer.Get(), 0);
    }
}

void Shader::UpdatePostProcessingConstants(const PostProcessingConstants& constants)
{
    ASSERT(m_context != nullptr);
    ASSERT(m_postProcessingBuffer != nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(m_postProcessingBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr))
    {
        auto dataPtr = reinterpret_cast<PostProcessingConstants*>(mapped.pData);
        *dataPtr = constants;
        m_context->Unmap(m_postProcessingBuffer.Get(), 0);
    }
}

void Shader::UpdateConstantBuffer(const ConstantBuffer& cb)
{
    ASSERT(m_context != nullptr);
    ASSERT(m_perObjectBuffer != nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(m_perObjectBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr))
    {
        ConstantBuffer* data = reinterpret_cast<ConstantBuffer*>(mapped.pData);
        data->World = XMMatrixTranspose(cb.World);
        data->View = XMMatrixTranspose(cb.View);
        data->Projection = XMMatrixTranspose(cb.Projection);

        m_context->Unmap(m_perObjectBuffer.Get(), 0);
    }
}

HRESULT Shader::CreateConstantBuffers()
{
    ASSERT(m_device != nullptr);

    LOG_TO_CONSOLE_IMMEDIATE(L"Creating shader constant buffers...", L"INFO");

    // Create per-frame constant buffer
    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.ByteWidth = sizeof(PerFrameConstants);
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = m_device->CreateBuffer(&bufferDesc, nullptr, m_perFrameBuffer.GetAddressOf());
    if (FAILED(hr))
    {
        std::wstring errorMsg = L"Failed to create per-frame constant buffer: HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    // Create per-object constant buffer
    bufferDesc.ByteWidth = sizeof(PerObjectConstants);
    hr = m_device->CreateBuffer(&bufferDesc, nullptr, m_perObjectBuffer.GetAddressOf());
    if (FAILED(hr))
    {
        std::wstring errorMsg = L"Failed to create per-object constant buffer: HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    // Create per-material constant buffer
    bufferDesc.ByteWidth = sizeof(PerMaterialConstants);
    hr = m_device->CreateBuffer(&bufferDesc, nullptr, m_perMaterialBuffer.GetAddressOf());
    if (FAILED(hr))
    {
        std::wstring errorMsg = L"Failed to create per-material constant buffer: HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    // Create lighting data constant buffer
    bufferDesc.ByteWidth = sizeof(LightingData);
    hr = m_device->CreateBuffer(&bufferDesc, nullptr, m_lightingDataBuffer.GetAddressOf());
    if (FAILED(hr))
    {
        std::wstring errorMsg = L"Failed to create lighting data constant buffer: HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    // Create post-processing constant buffer
    bufferDesc.ByteWidth = sizeof(PostProcessingConstants);
    hr = m_device->CreateBuffer(&bufferDesc, nullptr, m_postProcessingBuffer.GetAddressOf());
    if (FAILED(hr))
    {
        std::wstring errorMsg = L"Failed to create post-processing constant buffer: HR=0x" + std::to_wstring(hr);
        LOG_TO_CONSOLE_IMMEDIATE(errorMsg, L"ERROR");
        return hr;
    }

    // Log buffer sizes for debugging
    std::wstring sizeMsg = L"Constant buffer sizes: PerFrame=" + std::to_wstring(sizeof(PerFrameConstants)) +
                           L", PerObject=" + std::to_wstring(sizeof(PerObjectConstants)) + L", PerMaterial=" +
                           std::to_wstring(sizeof(PerMaterialConstants)) + L", Lighting=" +
                           std::to_wstring(sizeof(LightingData)) + L", PostProcess=" +
                           std::to_wstring(sizeof(PostProcessingConstants));
    LOG_TO_CONSOLE_IMMEDIATE(sizeMsg, L"DEBUG");

    LOG_TO_CONSOLE_IMMEDIATE(L"Shader constant buffers created successfully", L"SUCCESS");
    return S_OK;
}

#endif // SPARK_PLATFORM_WINDOWS
