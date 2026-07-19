/**
 * @file GraphicsDeviceResourcesWindowsInstanced.cpp
 * @brief D3D11 basic instanced draw path (W12 decor-instancing)
 *
 * Instanced permutation of the embedded basic vertex shader, instance buffer
 * management and DrawMeshInstanced, split out of
 * GraphicsDeviceResourcesWindows.cpp. Linux counterpart lives in
 * GraphicsDeviceResourcesLinuxShaders.cpp.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "GraphicsEngine.h"
#include "Mesh.h" // W12 decor-instancing: DrawMeshInstanced binds Mesh buffers directly
#include "../Utils/LogMacros.h"
#include "../Utils/SparkConsole.h"

#include <windows.h>
#include <d3d11_1.h>
#include <wrl.h>
#include <d3dcompiler.h>

#include <string>
#include <cstdint>
#include <cstring>

using Microsoft::WRL::ComPtr;

// ============================================================================
// BASIC INSTANCED DRAW PATH (W12 decor-instancing)
// ============================================================================
// A SECOND permutation of the embedded basic vertex shader that reads the
// world matrix from a per-instance vertex stream (IA slot 1) instead of the
// b0 cbuffer, letting repeated static meshes (region decor) collapse N draw
// calls into one DrawIndexedInstanced. Everything here is additive and lazy:
// the existing VS / input layout / SetBasicShaders() path is untouched, the
// pipeline objects are only created on the first HasInstancedBasicPipeline()
// probe, and a compile failure permanently reports "unavailable" so callers
// fall back to the per-entity path.
//
// SHADER MATH (identical to the non-instanced path by construction):
//  - Every cbuffer matrix is uploaded TRANSPOSED (XMMatrixTranspose) and
//    consumed as mul(rowVector, M) — the effective transform is v * W * V * P
//    in row-vector convention.
//  - The instance stream carries the world matrix UNTRANSPOSED: XMFLOAT4X4
//    row-major memory rows map 1:1 onto the four INSTWORLD float4 attributes,
//    and HLSL float4x4(r0, r1, r2, r3) builds its ROWS from them, so
//    mul(v, instWorld) == v * W — the same product the non-instanced VS gets
//    from its transposed cbuffer World. Clip position then applies the b1
//    ViewProjectionMatrix: (v * W) * (V * P), associativity-identical to the
//    non-instanced v * (W * V * P) WorldViewProjection.
//  - Normals use (float3x3)instWorld + normalize instead of the cbuffer
//    WorldInverseTranspose. For rotation + translation + UNIFORM scale
//    worlds, inverse-transpose(R) == R and the scale factor is normalized
//    away, so the lit result is identical (decor worlds are yaw+translation
//    only). NON-uniform scale would skew normals — callers must not use this
//    path for non-uniformly scaled instances.
//  - WorldPos, TexCoord (UVTiling), Color (ObjectColor) and the shared pixel
//    shader all read the same inputs as the non-instanced path, so lighting
//    is bit-comparable.

namespace
{
    /// Hard cap on instances per DrawIndexedInstanced upload (larger requests
    /// are split into chunks). 4096 * 64 B = 256 KB dynamic buffer worst case.
    constexpr uint32_t kMaxBasicInstances = 4096u;
    /// First allocation; grown geometrically (doubling) up to the cap.
    constexpr uint32_t kInitialBasicInstances = 256u;
} // namespace

HRESULT GraphicsEngine::CompileEmbeddedVertexShaderInstanced(ID3DBlob** blobOut)
{
    // Mirrors CompileEmbeddedVertexShader with ONE change: the world matrix
    // comes from the per-instance INSTWORLD attributes; b0's World/WVP/WIT
    // are ignored (ObjectColor/UVTiling are still read from b0, and the
    // shared pixel shader still reads MaterialProperties from b0).
    const char* vertexShaderSource = R"(
        cbuffer PerObjectConstants : register(b0)
        {
            matrix World;                   // unused by the instanced path
            matrix WorldViewProjection;     // unused by the instanced path
            matrix WorldInverseTranspose;   // unused by the instanced path
            matrix PreviousWorld;
            float3 ObjectPosition;
            float ObjectScale;
            float4 ObjectColor;
            float4 MaterialProperties;
            float4 UVTiling;
        };

        // Per-frame slot b1 — the instanced path takes view*proj from here
        // (the per-object WVP cannot exist when the world matrix varies per
        // instance). Must mirror Shader.h PerFrameConstants exactly.
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

        struct VertexInput
        {
            float3 Position : POSITION;
            float3 Normal   : NORMAL;
            float2 TexCoord : TEXCOORD0;
            // Per-instance world matrix rows (slot 1, step rate 1), uploaded
            // UNTRANSPOSED so float4x4(rows...) reproduces the XMMATRIX.
            float4 InstW0   : INSTWORLD0;
            float4 InstW1   : INSTWORLD1;
            float4 InstW2   : INSTWORLD2;
            float4 InstW3   : INSTWORLD3;
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

            float4x4 instWorld = float4x4(input.InstW0, input.InstW1, input.InstW2, input.InstW3);
            float4 worldPos = mul(float4(input.Position, 1.0f), instWorld); // v * W (row-vector)
            output.Position = mul(worldPos, ViewProjectionMatrix);          // (v*W) * (V*P)
            output.WorldPos = worldPos.xyz;
            // Valid for rotation + translation + uniform scale (see the
            // section header): inverse-transpose == the matrix itself for
            // the rotation part, and normalize() removes uniform scale.
            output.Normal = normalize(mul(input.Normal, (float3x3)instWorld));
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
    HRESULT hr = D3DCompile(vertexShaderSource, strlen(vertexShaderSource), "EmbeddedVertexShaderInstanced", nullptr,
                            nullptr, "main", "vs_5_0", shaderFlags, 0, blobOut, &errorBlob);

    if (FAILED(hr))
    {
        if (errorBlob)
        {
            std::string errorMsg(static_cast<const char*>(errorBlob->GetBufferPointer()));
            std::wstring wErrorMsg(errorMsg.begin(), errorMsg.end());
            LOG_TO_CONSOLE_IMMEDIATE(L"Embedded instanced vertex shader compilation error: " + wErrorMsg, L"ERROR");
        }
        return hr;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Embedded instanced vertex shader compiled successfully", L"SUCCESS");
    return S_OK;
}

bool GraphicsEngine::EnsureBasicInstancedPipeline()
{
    if (m_basicVertexShaderInstanced && m_basicInputLayoutInstanced)
        return true;
    if (m_basicInstancedTried) // one attempt per device: fail = permanently off
        return false;
    m_basicInstancedTried = true;

    // The instanced permutation piggybacks on the basic system (shared PS,
    // cbuffers, sampler, defaults) — require it to be initialized first.
    if (!m_device || !m_basicPixelShader || !m_basicConstantBuffer || !m_basicFrameConstantBuffer)
        return false;

    ComPtr<ID3DBlob> vsBlob;
    if (FAILED(CompileEmbeddedVertexShaderInstanced(&vsBlob)))
        return false;

    if (FAILED(m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                            &m_basicVertexShaderInstanced)))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create instanced vertex shader", L"ERROR");
        return false;
    }

    // Slot 0: the exact per-vertex layout the basic path uses. Slot 1: the
    // per-instance world matrix as four float4 rows, step rate 1.
    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"INSTWORLD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INSTWORLD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INSTWORLD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INSTWORLD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };

    if (FAILED(m_device->CreateInputLayout(layout, ARRAYSIZE(layout), vsBlob->GetBufferPointer(),
                                           vsBlob->GetBufferSize(), &m_basicInputLayoutInstanced)))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to create instanced input layout", L"ERROR");
        m_basicVertexShaderInstanced.Reset();
        return false;
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Basic instanced pipeline created", L"SUCCESS");
    return true;
}

bool GraphicsEngine::HasInstancedBasicPipeline()
{
    return EnsureBasicInstancedPipeline();
}

void GraphicsEngine::SetBasicShadersInstanced()
{
    if (!m_context || !EnsureBasicInstancedPipeline())
        return;

    // Instanced VS + the SHARED basic pixel shader (no PS changes).
    m_context->VSSetShader(m_basicVertexShaderInstanced.Get(), nullptr, 0);
    m_context->PSSetShader(m_basicPixelShader.Get(), nullptr, 0);
    m_context->IASetInputLayout(m_basicInputLayoutInstanced.Get());

    // Identical resource bindings to SetBasicShaders(): per-object b0
    // (ObjectColor/UVTiling/MaterialProperties still per-GROUP via
    // UpdateBasicConstants), per-frame b1 (view*proj source), sampler and
    // texture defaults.
    m_context->VSSetConstantBuffers(0, 1, m_basicConstantBuffer.GetAddressOf());
    m_context->VSSetConstantBuffers(1, 1, m_basicFrameConstantBuffer.GetAddressOf());
    m_context->PSSetConstantBuffers(0, 1, m_basicConstantBuffer.GetAddressOf());
    m_context->PSSetConstantBuffers(1, 1, m_basicFrameConstantBuffer.GetAddressOf());
    m_context->PSSetSamplers(0, 1, m_basicSamplerState.GetAddressOf());
    if (m_defaultSRV)
    {
        m_context->PSSetShaderResources(0, 1, m_defaultSRV.GetAddressOf());
    }
    SetBasicMaterialTextures(nullptr, nullptr);
}

bool GraphicsEngine::EnsureBasicInstanceCapacity(uint32_t instanceCount)
{
    if (!m_device || instanceCount == 0 || instanceCount > kMaxBasicInstances)
        return false;
    if (m_basicInstanceBuffer && m_basicInstanceCapacity >= instanceCount)
        return true;

    // Geometric growth, capped: 256 -> 512 -> ... -> 4096 matrices.
    uint32_t capacity = (m_basicInstanceCapacity > 0) ? m_basicInstanceCapacity : kInitialBasicInstances;
    while (capacity < instanceCount)
        capacity *= 2u;
    if (capacity > kMaxBasicInstances)
        capacity = kMaxBasicInstances;

    D3D11_BUFFER_DESC desc = {};
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.ByteWidth = capacity * static_cast<UINT>(sizeof(DirectX::XMFLOAT4X4));
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    ComPtr<ID3D11Buffer> buffer;
    if (FAILED(m_device->CreateBuffer(&desc, nullptr, &buffer)))
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Failed to (re)create basic instance buffer", L"ERROR");
        return false;
    }
    m_basicInstanceBuffer = std::move(buffer);
    m_basicInstanceCapacity = capacity;
    return true;
}

bool GraphicsEngine::DrawMeshInstanced(Mesh& mesh, const DirectX::XMFLOAT4X4* instanceWorlds, uint32_t instanceCount,
                                       uint32_t indexStart, uint32_t indexCount)
{
    // Caller contract: SetBasicShadersInstanced() bound the pipeline and
    // UpdateBasicConstants(...) set the per-group b0 constants (the world
    // part of b0 is ignored — instance matrices replace it). Instance worlds
    // must be rotation+translation+uniform-scale (see the section header).
    if (!m_context || !instanceWorlds || instanceCount == 0)
        return false;
    if (!EnsureBasicInstancedPipeline())
        return false;

    ID3D11Buffer* vb = mesh.GetVertexBuffer();
    ID3D11Buffer* ib = mesh.GetIndexBuffer();
    if (!vb || !ib || mesh.GetIndexCount() == 0)
        return false;
    if (indexCount == 0) // 0 = whole mesh (RenderRange semantics otherwise)
    {
        indexStart = 0;
        indexCount = mesh.GetIndexCount();
    }

    // Upload + draw in chunks of kMaxBasicInstances (one chunk in practice —
    // decor groups are tens of instances; the cap bounds the dynamic buffer).
    for (uint32_t base = 0; base < instanceCount; base += kMaxBasicInstances)
    {
        const uint32_t remaining = instanceCount - base;
        const uint32_t n = (remaining < kMaxBasicInstances) ? remaining : kMaxBasicInstances;
        if (!EnsureBasicInstanceCapacity(n))
            return false;

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(m_context->Map(m_basicInstanceBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return false;
        memcpy(mapped.pData, instanceWorlds + base, static_cast<size_t>(n) * sizeof(DirectX::XMFLOAT4X4));
        m_context->Unmap(m_basicInstanceBuffer.Get(), 0);

        // Same bindings Mesh::Render/RenderRange use for slot 0, plus the
        // instance stream at slot 1 (the non-instanced input layout never
        // references slot 1, so a stale binding there is inert).
        ID3D11Buffer* vbs[2] = {vb, m_basicInstanceBuffer.Get()};
        const UINT strides[2] = {sizeof(Vertex), sizeof(DirectX::XMFLOAT4X4)};
        const UINT offsets[2] = {0, 0};
        m_context->IASetVertexBuffers(0, 2, vbs, strides, offsets);
        m_context->IASetIndexBuffer(ib, DXGI_FORMAT_R32_UINT, 0);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->DrawIndexedInstanced(indexCount, n, indexStart, 0, 0);
    }
    return true;
}
#endif // SPARK_PLATFORM_WINDOWS
