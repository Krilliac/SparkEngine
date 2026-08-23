/**
 * @file TestGPUDrivenRendererD3D11.cpp
 * @brief Production-linked ABI and live WARP coverage for GPU culling.
 */
#include "TestFramework.h"
#include "Graphics/GPUDrivenRenderer.h"

#include <DirectXMath.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace Spark::Graphics;

namespace
{
    class ScopedCurrentDirectory
    {
      public:
        explicit ScopedCurrentDirectory(const wchar_t* path)
        {
            DWORD required = GetCurrentDirectoryW(0, nullptr);
            m_previous.resize(required);
            GetCurrentDirectoryW(required, m_previous.data());
            SetCurrentDirectoryW(path);
        }

        ~ScopedCurrentDirectory()
        {
            if (!m_previous.empty())
                SetCurrentDirectoryW(m_previous.c_str());
        }

      private:
        std::wstring m_previous;
    };

    ComPtr<ID3DBlob> CompileCullShader()
    {
        ComPtr<ID3DBlob> shader;
        ComPtr<ID3DBlob> errors;
        HRESULT hr = D3DCompileFromFile(L"Shaders/HLSL/Compute/GPUCull.hlsl", nullptr,
                                        D3D_COMPILE_STANDARD_FILE_INCLUDE, "CSMain", "cs_5_0",
                                        D3DCOMPILE_ENABLE_STRICTNESS, 0, shader.GetAddressOf(), errors.GetAddressOf());
        if (FAILED(hr) && errors)
            std::cerr << static_cast<const char*>(errors->GetBufferPointer()) << '\n';
        return SUCCEEDED(hr) ? shader : nullptr;
    }

    bool ExpectBinding(ID3D11ShaderReflection* reflection, const char* name, D3D_SHADER_INPUT_TYPE type, UINT bindPoint)
    {
        D3D11_SHADER_INPUT_BIND_DESC binding{};
        if (FAILED(reflection->GetResourceBindingDescByName(name, &binding)))
            return false;
        return binding.Type == type && binding.BindPoint == bindPoint && binding.BindCount == 1;
    }

    bool CreateWarpDevice(ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context)
    {
        D3D_FEATURE_LEVEL featureLevel{};
        return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                           device.GetAddressOf(), &featureLevel, context.GetAddressOf()));
    }

    struct RendererShutdownGuard
    {
        ~RendererShutdownGuard() { GPUDrivenRenderer::GetInstance().Shutdown(); }
    };
} // namespace

TEST(GPUDriven_D3D11_ShaderReflectionMatchesSharedABI)
{
    ScopedCurrentDirectory sourceDirectory(L"" SPARK_TEST_SOURCE_DIR_WIDE);
    ComPtr<ID3DBlob> shader = CompileCullShader();
    ASSERT_TRUE(shader != nullptr);

    ComPtr<ID3D11ShaderReflection> reflection;
    ASSERT_TRUE(SUCCEEDED(D3DReflect(shader->GetBufferPointer(), shader->GetBufferSize(), IID_ID3D11ShaderReflection,
                                     reinterpret_cast<void**>(reflection.GetAddressOf()))));

    EXPECT_TRUE(ExpectBinding(reflection.Get(), "GPUCullConstants", D3D_SIT_CBUFFER, 0));
    EXPECT_TRUE(ExpectBinding(reflection.Get(), "boundingBoxes", D3D_SIT_STRUCTURED, 0));
    EXPECT_TRUE(ExpectBinding(reflection.Get(), "hiZTexture", D3D_SIT_TEXTURE, 1));
    EXPECT_TRUE(ExpectBinding(reflection.Get(), "visibilityFlags", D3D_SIT_UAV_RWSTRUCTURED, 0));
    EXPECT_TRUE(ExpectBinding(reflection.Get(), "indirectArgs", D3D_SIT_UAV_RWBYTEADDRESS, 1));

    ID3D11ShaderReflectionConstantBuffer* constants = reflection->GetConstantBufferByName("GPUCullConstants");
    ASSERT_TRUE(constants != nullptr);
    D3D11_SHADER_BUFFER_DESC bufferDesc{};
    ASSERT_TRUE(SUCCEEDED(constants->GetDesc(&bufferDesc)));
    EXPECT_EQ(bufferDesc.Size, static_cast<UINT>(sizeof(GPUCullConstants)));

    D3D11_SHADER_VARIABLE_DESC variableDesc{};
    ASSERT_TRUE(SUCCEEDED(constants->GetVariableByName("instanceCount")->GetDesc(&variableDesc)));
    EXPECT_EQ(variableDesc.StartOffset, static_cast<UINT>(offsetof(GPUCullConstants, instanceCount)));
    ASSERT_TRUE(SUCCEEDED(constants->GetVariableByName("enableFrustumCull")->GetDesc(&variableDesc)));
    EXPECT_EQ(variableDesc.StartOffset, static_cast<UINT>(offsetof(GPUCullConstants, enableFrustumCull)));
}

TEST(GPUDriven_D3D11_PrimitiveLosesSparseSourceIdentityAndProductionGateFailsClosed)
{
    ScopedCurrentDirectory sourceDirectory(L"" SPARK_TEST_SOURCE_DIR_WIDE);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ASSERT_TRUE(CreateWarpDevice(device, context));

    auto& renderer = GPUDrivenRenderer::GetInstance();
    renderer.Shutdown();
    RendererShutdownGuard shutdownGuard;
    ASSERT_TRUE(renderer.Initialize(device.Get(), context.Get(), 2));
    renderer.GetSettings().enableFrustumCull = true;
    renderer.GetSettings().enableHiZCull = false;
    renderer.GetSettings().freezeCulling = false;

    const std::array<XMFLOAT3, 3> vertices = {XMFLOAT3{-0.25f, -0.25f, 0.5f}, XMFLOAT3{0.0f, 0.25f, 0.5f},
                                              XMFLOAT3{0.25f, -0.25f, 0.5f}};
    const std::array<uint32_t, 3> indices = {0, 1, 2};

    D3D11_BUFFER_DESC vertexDesc{};
    vertexDesc.ByteWidth = sizeof(vertices);
    vertexDesc.Usage = D3D11_USAGE_IMMUTABLE;
    vertexDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vertexData{vertices.data()};
    ComPtr<ID3D11Buffer> vertexBuffer;
    ASSERT_TRUE(SUCCEEDED(device->CreateBuffer(&vertexDesc, &vertexData, vertexBuffer.GetAddressOf())));

    D3D11_BUFFER_DESC indexDesc{};
    indexDesc.ByteWidth = sizeof(indices);
    indexDesc.Usage = D3D11_USAGE_IMMUTABLE;
    indexDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA indexData{indices.data()};
    ComPtr<ID3D11Buffer> indexBuffer;
    ASSERT_TRUE(SUCCEEDED(device->CreateBuffer(&indexDesc, &indexData, indexBuffer.GetAddressOf())));

    static constexpr char vertexShaderSource[] =
        "struct In { float3 position : POSITION; };"
        "struct Out { float4 position : SV_Position; nointerpolation uint sourceInstance : TEXCOORD0; };"
        "Out VSMain(In input, uint instanceID : SV_InstanceID) {"
        "  Out output; output.position = float4(input.position, 1.0);"
        "  output.sourceInstance = instanceID; return output; }";
    ComPtr<ID3DBlob> vertexShaderBlob;
    ASSERT_TRUE(
        SUCCEEDED(D3DCompile(vertexShaderSource, sizeof(vertexShaderSource), nullptr, nullptr, nullptr, "VSMain",
                             "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, vertexShaderBlob.GetAddressOf(), nullptr)));
    ComPtr<ID3D11VertexShader> vertexShader;
    ASSERT_TRUE(
        SUCCEEDED(device->CreateVertexShader(vertexShaderBlob->GetBufferPointer(), vertexShaderBlob->GetBufferSize(),
                                             nullptr, vertexShader.GetAddressOf())));
    D3D11_INPUT_ELEMENT_DESC positionElement = {
        "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    ComPtr<ID3D11InputLayout> inputLayout;
    ASSERT_TRUE(SUCCEEDED(device->CreateInputLayout(&positionElement, 1, vertexShaderBlob->GetBufferPointer(),
                                                    vertexShaderBlob->GetBufferSize(), inputLayout.GetAddressOf())));
    context->IASetInputLayout(inputLayout.Get());
    context->VSSetShader(vertexShader.Get(), nullptr, 0);

    static constexpr char geometryShaderSource[] =
        "struct Vertex { float4 position : SV_Position; nointerpolation uint sourceInstance : TEXCOORD0; };"
        "[maxvertexcount(3)] void GSMain(triangle Vertex input[3], inout TriangleStream<Vertex> output) {"
        "  output.Append(input[0]); output.Append(input[1]); output.Append(input[2]); }";
    ComPtr<ID3DBlob> geometryShaderBlob;
    ASSERT_TRUE(
        SUCCEEDED(D3DCompile(geometryShaderSource, sizeof(geometryShaderSource), nullptr, nullptr, nullptr, "GSMain",
                             "gs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, geometryShaderBlob.GetAddressOf(), nullptr)));
    const D3D11_SO_DECLARATION_ENTRY streamOutputDeclaration = {0, "TEXCOORD", 0, 0, 1, 0};
    const UINT streamOutputStride = sizeof(uint32_t);
    ComPtr<ID3D11GeometryShader> geometryShader;
    ASSERT_TRUE(SUCCEEDED(device->CreateGeometryShaderWithStreamOutput(
        geometryShaderBlob->GetBufferPointer(), geometryShaderBlob->GetBufferSize(), &streamOutputDeclaration, 1,
        &streamOutputStride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, geometryShader.GetAddressOf())));
    context->GSSetShader(geometryShader.Get(), nullptr, 0);

    D3D11_BUFFER_DESC streamOutputDesc{};
    streamOutputDesc.ByteWidth = 3 * sizeof(uint32_t);
    streamOutputDesc.Usage = D3D11_USAGE_DEFAULT;
    streamOutputDesc.BindFlags = D3D11_BIND_STREAM_OUTPUT;
    ComPtr<ID3D11Buffer> streamOutput;
    ASSERT_TRUE(SUCCEEDED(device->CreateBuffer(&streamOutputDesc, nullptr, streamOutput.GetAddressOf())));
    ID3D11Buffer* streamOutputTarget = streamOutput.Get();
    UINT streamOutputOffset = 0;
    context->SOSetTargets(1, &streamOutputTarget, &streamOutputOffset);

    D3D11_QUERY_DESC queryDesc{D3D11_QUERY_PIPELINE_STATISTICS, 0};
    ComPtr<ID3D11Query> query;
    ASSERT_TRUE(SUCCEEDED(device->CreateQuery(&queryDesc, query.GetAddressOf())));

    // These are the transformed world bounds of two otherwise-identical
    // instances. Source instance 0 is outside the identity frustum; only
    // source instance 1 survives.
    XMFLOAT4X4 sourceWorlds[2];
    XMStoreFloat4x4(&sourceWorlds[0], XMMatrixTranslation(2.5f, 0.0f, 0.0f));
    XMStoreFloat4x4(&sourceWorlds[1], XMMatrixTranslation(0.0f, 0.0f, 0.0f));
    const std::array<GPUInstanceAABB, 2> boxes = {
        GPUInstanceAABB{sourceWorlds[0]._41 - 0.2f, -0.2f, 0.1f, 0.0f, sourceWorlds[0]._41 + 0.2f, 0.2f, 0.3f, 0.0f},
        GPUInstanceAABB{sourceWorlds[1]._41 - 0.2f, -0.2f, 0.1f, 0.0f, sourceWorlds[1]._41 + 0.2f, 0.2f, 0.3f, 0.0f}};

    context->Begin(query.Get());
    EXPECT_TRUE(renderer.CullAndDraw(boxes.data(), static_cast<uint32_t>(boxes.size()), XMMatrixIdentity(),
                                     XMMatrixIdentity(), indexBuffer.Get(), vertexBuffer.Get(), sizeof(XMFLOAT3),
                                     static_cast<uint32_t>(indices.size())));
    context->End(query.Get());
    context->Flush();

    ID3D11Buffer* nullStreamOutput = nullptr;
    UINT nullStreamOutputOffset = 0;
    context->SOSetTargets(1, &nullStreamOutput, &nullStreamOutputOffset);

    D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
    HRESULT queryResult = S_FALSE;
    for (int attempt = 0; attempt < 10000 && queryResult == S_FALSE; ++attempt)
    {
        queryResult = context->GetData(query.Get(), &stats, sizeof(stats), 0);
        if (queryResult == S_FALSE)
            std::this_thread::yield();
    }
    ASSERT_TRUE(SUCCEEDED(queryResult) && queryResult != S_FALSE);
    EXPECT_EQ(stats.VSInvocations, static_cast<UINT64>(3));

    D3D11_BUFFER_DESC readbackDesc = streamOutputDesc;
    readbackDesc.Usage = D3D11_USAGE_STAGING;
    readbackDesc.BindFlags = 0;
    readbackDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> readback;
    ASSERT_TRUE(SUCCEEDED(device->CreateBuffer(&readbackDesc, nullptr, readback.GetAddressOf())));
    context->CopyResource(readback.Get(), streamOutput.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ASSERT_TRUE(SUCCEEDED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
    const auto* consumedInstanceIDs = static_cast<const uint32_t*>(mapped.pData);
    EXPECT_EQ(consumedInstanceIDs[0], 0u);
    EXPECT_EQ(consumedInstanceIDs[1], 0u);
    EXPECT_EQ(consumedInstanceIDs[2], 0u);
    context->Unmap(readback.Get(), 0);

    // The survivor was source index 1, while the graphics shader consumed
    // SV_InstanceID 0. The isolated primitive therefore cannot preserve
    // sparse source identity; the production draw list must stay CPU-routed.
    EXPECT_FALSE(GPUDrivenRenderer::SupportsProductionDrawListInstanceContract());

    EXPECT_FALSE(renderer.CullAndDraw(boxes.data(), 2, XMMatrixIdentity(), XMMatrixIdentity(), nullptr,
                                      vertexBuffer.Get(), sizeof(XMFLOAT3), 3));
    EXPECT_FALSE(renderer.CullAndDraw(boxes.data(), 3, XMMatrixIdentity(), XMMatrixIdentity(), indexBuffer.Get(),
                                      vertexBuffer.Get(), sizeof(XMFLOAT3), 3));
    renderer.GetSettings().freezeCulling = true;
    EXPECT_FALSE(renderer.CullAndDraw(boxes.data(), 2, XMMatrixIdentity(), XMMatrixIdentity(), indexBuffer.Get(),
                                      vertexBuffer.Get(), sizeof(XMFLOAT3), 3));
}
