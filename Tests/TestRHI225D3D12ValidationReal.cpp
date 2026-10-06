/**
 * @file TestRHI225D3D12ValidationReal.cpp
 * @brief RHI-225: the D3D12 RHI renders and churns resources with zero debug-layer errors.
 *
 * Every test initializes D3D12Device with the debug layer and GPU-based validation, drives it
 * through the IRHIDevice/IRHICommandList interface and reads D3D12Device::GetValidationCounts().
 * A device without an active info queue fails the test rather than passing it with nothing
 * counted, and the counter is shown to see an injected invalid call, so a clean count means
 * clean. Scope: the declared RHI-level set below, not engine frames (GraphicsEngine is
 * D3D11-direct on Windows). The lane (CTest D3D12_Validation) is excluded from the main suite
 * because enabling the debug layer is process-wide.
 *
 * The constant-buffer and sampled-texture draws pin resource binding: a root CBV per b-slot
 * and SRV/sampler tables copied into shader-visible pages per draw. Before that path existed
 * SetConstantBuffer targeted a descriptor-table parameter (a debug-layer error) and
 * SetShaderResource/SetSampler did nothing, so a textured draw sampled nothing.
 */

#include "TestFramework.h"

#if defined(_WIN32) && !defined(SPARK_NO_D3D12)

#include "Graphics/RHI/D3D12/D3D12Device.h"
#include "Graphics/RHI/RHIPipelineTypes.h"
#include "Graphics/RHI/RHIResources.h"
#include "Graphics/RHI/RHITypes.h"

#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

namespace
{
    using Microsoft::WRL::ComPtr;
    using namespace Spark::RHI;
    using Spark::RHI::D3D12::D3D12CommandList;
    using Spark::RHI::D3D12::D3D12Device;
    using Spark::RHI::D3D12::D3D12Texture;
    using Spark::RHI::D3D12::D3D12ValidationCounts;

    constexpr uint32_t kSize = 64;
    constexpr uint32_t kRowPitch = kSize * 4; // 256: already D3D12_TEXTURE_DATA_PITCH_ALIGNMENT

    constexpr const char* kVertexShader = R"(
float4 main(float3 position : POSITION) : SV_Position
{
    return float4(position, 1.0);
}
)";

    constexpr const char* kPixelShader = R"(
float4 main() : SV_Target
{
    return float4(1.0, 0.0, 0.0, 1.0);
}
)";

    constexpr const char* kTexturedVertexShader = R"(
struct Output
{
    float2 uv : TEXCOORD;
    float4 position : SV_Position;
};
Output main(float3 position : POSITION, float2 uv : TEXCOORD)
{
    Output output;
    output.uv = uv;
    output.position = float4(position, 1.0);
    return output;
}
)";

    constexpr const char* kConstantColorPixelShader = R"(
cbuffer Material : register(b0)
{
    float4 color;
};
float4 main() : SV_Target
{
    return color;
}
)";

    constexpr const char* kSampledPixelShader = R"(
Texture2D source : register(t0);
SamplerState pointSampler : register(s0);
float4 main(float2 uv : TEXCOORD) : SV_Target
{
    return source.Sample(pointSampler, uv);
}
)";

    /// Debug layer + GPU-based validation. Asserts the info queue is live so the lane cannot pass empty.
    void InitializeValidated(D3D12Device& device)
    {
        RHIDeviceDesc desc;
        desc.enableDebugLayer = true;
        desc.enableGPUValidation = true;
        desc.applicationName = "SparkTests_RHI225_D3D12Validation";
        ASSERT_TRUE(device.Initialize(desc));
        ASSERT_TRUE(device.GetValidationCounts().active);
    }

    /// Prints every stored warning-or-worse message so a failing count names its cause.
    void DumpMessages(ID3D12Device* device)
    {
        ComPtr<ID3D12InfoQueue> queue;
        if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))))
            return;
        for (UINT64 i = 0; i < queue->GetNumStoredMessages(); ++i)
        {
            SIZE_T length = 0;
            if (FAILED(queue->GetMessage(i, nullptr, &length)) || length == 0)
                continue;
            std::vector<uint64_t> storage((length + sizeof(uint64_t) - 1) / sizeof(uint64_t));
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(queue->GetMessage(i, message, &length)))
                std::fprintf(stderr, "  D3D12 [severity %d, id %d] %s\n", static_cast<int>(message->Severity),
                             static_cast<int>(message->ID), message->pDescription);
        }
    }

    void ExpectClean(D3D12Device& device)
    {
        const D3D12ValidationCounts counts = device.GetValidationCounts();
        EXPECT_TRUE(counts.active);
        EXPECT_EQ(counts.corruption, 0u);
        EXPECT_EQ(counts.errors, 0u);
        // A non-zero discard count would make the zeros above a floor, not a total.
        EXPECT_EQ(counts.discarded, 0u);
        if (counts.corruption != 0 || counts.errors != 0 || counts.discarded != 0)
            DumpMessages(device.GetD3D12Device());
    }

    /// A triangle (or, textured, a full-target quad with UVs), a pipeline around @p pixelShaderSource,
    /// and a colour target with a READBACK copy.
    struct TriangleScene
    {
        std::unique_ptr<IRHIShader> vertexShader;
        std::unique_ptr<IRHIShader> pixelShader;
        std::unique_ptr<IRHIPipelineState> pipeline;
        std::unique_ptr<IRHIBuffer> vertices;
        std::unique_ptr<IRHITexture> target;
        ComPtr<ID3D12Resource> readback;
        uint32_t vertexCount = 3;

        explicit TriangleScene(D3D12Device& device, const char* pixelShaderSource = kPixelShader, bool textured = false)
        {
            RHIShaderDesc vs;
            vs.stage = RHIShaderStage::Vertex;
            vs.sourceCode = textured ? kTexturedVertexShader : kVertexShader;
            vs.filePath = "RHI225TriangleVS";
            vertexShader = device.CreateShader(vs);
            RHIShaderDesc ps;
            ps.stage = RHIShaderStage::Pixel;
            ps.sourceCode = pixelShaderSource;
            ps.filePath = "RHI225TrianglePS";
            pixelShader = device.CreateShader(ps);
            ASSERT_TRUE(vertexShader != nullptr && pixelShader != nullptr);

            RHIPipelineStateDesc pso;
            RHIInputElement position;
            position.semanticName = "POSITION";
            position.format = RHIVertexFormat::Float3;
            pso.inputLayout.elements.push_back(position);
            if (textured)
            {
                RHIInputElement uv;
                uv.semanticName = "TEXCOORD";
                uv.format = RHIVertexFormat::Float2;
                uv.byteOffset = 3 * sizeof(float);
                pso.inputLayout.elements.push_back(uv);
            }
            pso.rasterizer.cullMode = RHICullMode::None;
            pso.depthStencil.depthEnable = false;
            pso.depthStencil.depthWrite = false;
            pso.depthStencilFormat = PixelFormat::Unknown;
            pso.numRenderTargets = 1;
            pso.renderTargetFormats[0] = PixelFormat::R8G8B8A8_UNORM;
            pipeline = device.CreatePipelineState(pso, vertexShader.get(), pixelShader.get());
            ASSERT_TRUE(pipeline != nullptr);

            // The triangle covers the centre pixel and leaves the corners at the clear colour;
            // the textured quad covers the whole target, UV (0,0) at the top-left.
            const float triangle[9] = {-0.5f, -0.5f, 0.0f, 0.0f, 0.5f, 0.0f, 0.5f, -0.5f, 0.0f};
            const float quad[30] = {-1.0f, 1.0f,  0.0f, 0.0f, 0.0f, 1.0f,  1.0f,  0.0f, 1.0f, 0.0f,
                                    -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, -1.0f, 0.0f, 0.0f, 1.0f,
                                    1.0f,  1.0f,  0.0f, 1.0f, 0.0f, 1.0f,  -1.0f, 0.0f, 1.0f, 1.0f};
            vertexCount = textured ? 6u : 3u;
            RHIBufferDesc vb;
            vb.size = textured ? sizeof(quad) : sizeof(triangle);
            vb.stride = (textured ? 5u : 3u) * static_cast<uint32_t>(sizeof(float));
            vb.usage = RHIBufferUsage::Vertex;
            vb.access = RHIBufferAccess::Dynamic;
            vb.initialData = textured ? static_cast<const void*>(quad) : static_cast<const void*>(triangle);
            vb.debugName = "RHI225TriangleVB";
            vertices = device.CreateBuffer(vb);
            ASSERT_TRUE(vertices != nullptr);

            RHITextureDesc color;
            color.width = kSize;
            color.height = kSize;
            color.format = PixelFormat::R8G8B8A8_UNORM;
            color.usage = RHITextureUsage::RenderTarget;
            color.debugName = "RHI225TriangleTarget";
            target = device.CreateTexture(color);
            ASSERT_TRUE(target != nullptr && target->GetRenderTargetView() != nullptr);

            D3D12_HEAP_PROPERTIES heap = {};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer = {};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = static_cast<UINT64>(kRowPitch) * kSize;
            buffer.Height = 1;
            buffer.DepthOrArraySize = 1;
            buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ASSERT_TRUE(SUCCEEDED(device.GetD3D12Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                                                   D3D12_RESOURCE_STATE_COPY_DEST,
                                                                                   nullptr, IID_PPV_ARGS(&readback))));
        }

        /// Records clear + draw + copy-to-readback on the immediate list, submits it and waits.
        /// @p bind runs after the pipeline is set, before the draw.
        void RenderFrame(D3D12Device& device, const std::function<void(IRHICommandList&)>& bind = {})
        {
            device.BeginFrame();
            auto* cmd = static_cast<D3D12CommandList*>(device.GetImmediateCommandList());
            auto* color = static_cast<D3D12Texture*>(target.get());
            cmd->Begin();
            cmd->TransitionBarrier(color, color->GetCurrentState(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmd->FlushBarriers();
            IRHITexture* targets[] = {target.get()};
            cmd->SetRenderTargets(targets, 1, nullptr);
            const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            cmd->ClearRenderTarget(target.get(), black);
            RHIViewport viewport;
            viewport.width = static_cast<float>(kSize);
            viewport.height = static_cast<float>(kSize);
            cmd->SetViewport(viewport);
            RHIScissorRect scissor;
            scissor.right = static_cast<int32_t>(kSize);
            scissor.bottom = static_cast<int32_t>(kSize);
            cmd->SetScissorRect(scissor);
            cmd->SetPipelineState(pipeline.get());
            cmd->SetPrimitiveTopology(RHIPrimitiveTopology::TriangleList);
            cmd->SetVertexBuffer(vertices.get(), 0, 0);
            if (bind)
                bind(*cmd);
            cmd->Draw(vertexCount, 0);

            cmd->TransitionBarrier(color, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->FlushBarriers();
            D3D12_TEXTURE_COPY_LOCATION source = {};
            source.pResource = color->GetD3D12Resource();
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION destination = {};
            destination.pResource = readback.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            destination.PlacedFootprint.Footprint.Width = kSize;
            destination.PlacedFootprint.Footprint.Height = kSize;
            destination.PlacedFootprint.Footprint.Depth = 1;
            destination.PlacedFootprint.Footprint.RowPitch = kRowPitch;
            cmd->GetCommandList()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            cmd->TransitionBarrier(color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            cmd->End();
            device.ExecuteCommandList(cmd);
            device.WaitForIdle();
            device.EndFrame();
        }

        /// RGBA8 texel of the last rendered frame.
        uint32_t Pixel(uint32_t x, uint32_t y) const
        {
            const D3D12_RANGE readRange = {0, static_cast<SIZE_T>(kRowPitch) * kSize};
            void* mapped = nullptr;
            if (FAILED(readback->Map(0, &readRange, &mapped)) || !mapped)
                return 0xDEADBEEFu;
            uint32_t texel = 0;
            std::memcpy(&texel, static_cast<const uint8_t*>(mapped) + y * kRowPitch + x * 4, sizeof(texel));
            const D3D12_RANGE noWrite = {0, 0};
            readback->Unmap(0, &noWrite);
            return texel;
        }
    };

    constexpr uint32_t kOpaqueRed = 0xFF0000FFu;   // R=255, A=255 in little-endian RGBA8
    constexpr uint32_t kOpaqueBlack = 0xFF000000u; // A=255
} // namespace

TEST(D3D12_Validation_TriangleFrameIsClean)
{
    D3D12Device device;
    InitializeValidated(device);
    {
        TriangleScene scene(device);
        // Two frames: the second re-records the same pipeline into a reset command list.
        for (int frame = 0; frame < 2; ++frame)
        {
            scene.RenderFrame(device);
            // The frame really drew: a clean count over an empty frame would prove nothing.
            EXPECT_EQ(scene.Pixel(kSize / 2, kSize / 2), kOpaqueRed);
            EXPECT_EQ(scene.Pixel(0, 0), kOpaqueBlack);
        }
        EXPECT_TRUE(SUCCEEDED(device.GetD3D12Device()->GetDeviceRemovedReason()));
        ExpectClean(device);
    }
    device.Shutdown();
}

TEST(D3D12_Validation_ConstantBufferColorDrawIsClean)
{
    D3D12Device device;
    InitializeValidated(device);
    {
        TriangleScene scene(device, kConstantColorPixelShader);
        // Exact 8-bit values (51, 102, 204) so the probe needs no tolerance.
        std::array<float, 64> constants = {}; // 256 bytes: one CBV-sized block
        constants[0] = 51.0f / 255.0f;
        constants[1] = 102.0f / 255.0f;
        constants[2] = 204.0f / 255.0f;
        constants[3] = 1.0f;
        RHIBufferDesc cb;
        cb.size = sizeof(constants);
        cb.usage = RHIBufferUsage::Constant;
        cb.access = RHIBufferAccess::Dynamic;
        cb.initialData = constants.data();
        cb.debugName = "RHI225MaterialCB";
        auto material = device.CreateBuffer(cb);
        ASSERT_TRUE(material != nullptr);

        for (int frame = 0; frame < 2; ++frame)
        {
            scene.RenderFrame(device, [&](IRHICommandList& cmd)
                              { cmd.SetConstantBuffer(RHIShaderStage::Pixel, 0, material.get()); });
            EXPECT_EQ(scene.Pixel(kSize / 2, kSize / 2), 0xFFCC6633u); // R=0x33 G=0x66 B=0xCC A=0xFF
            EXPECT_EQ(scene.Pixel(0, 0), kOpaqueBlack);
        }
        EXPECT_TRUE(SUCCEEDED(device.GetD3D12Device()->GetDeviceRemovedReason()));
        ExpectClean(device);
    }
    device.Shutdown();
}

TEST(D3D12_Validation_SampledTextureDrawIsClean)
{
    D3D12Device device;
    InitializeValidated(device);
    {
        TriangleScene scene(device, kSampledPixelShader, /*textured=*/true);

        // 4x4 texels, all distinct; the full-target quad maps each to a 16x16 block.
        constexpr uint32_t kTexels = 4;
        const auto texel = [](uint32_t x, uint32_t y)
        { return 0xFF000000u | ((x + y) * 20u) << 16 | (40u + 50u * y) << 8 | (40u + 50u * x); };
        std::vector<uint32_t> texels(kTexels * kTexels);
        for (uint32_t y = 0; y < kTexels; ++y)
            for (uint32_t x = 0; x < kTexels; ++x)
                texels[y * kTexels + x] = texel(x, y);
        RHITextureDesc desc;
        desc.width = kTexels;
        desc.height = kTexels;
        desc.format = PixelFormat::R8G8B8A8_UNORM;
        desc.usage = RHITextureUsage::ShaderResource;
        desc.debugName = "RHI225SampledTexture";
        auto texture = device.CreateTexture(desc);
        ASSERT_TRUE(texture != nullptr);
        device.UpdateTexture(texture.get(), texels.data(), 0, 0);

        RHISamplerDesc pointClamp;
        pointClamp.minFilter = RHIFilterMode::Nearest;
        pointClamp.magFilter = RHIFilterMode::Nearest;
        pointClamp.mipFilter = RHIFilterMode::Nearest;
        pointClamp.addressU = RHIAddressMode::Clamp;
        pointClamp.addressV = RHIAddressMode::Clamp;
        pointClamp.addressW = RHIAddressMode::Clamp;
        auto sampler = device.CreateSampler(pointClamp);
        ASSERT_TRUE(sampler != nullptr);

        for (int frame = 0; frame < 2; ++frame)
        {
            scene.RenderFrame(device,
                              [&](IRHICommandList& cmd)
                              {
                                  cmd.SetShaderResource(RHIShaderStage::Pixel, 0, texture.get());
                                  cmd.SetSampler(RHIShaderStage::Pixel, 0, sampler.get());
                              });
            const uint32_t block = kSize / kTexels;
            for (uint32_t y = 0; y < kTexels; ++y)
                for (uint32_t x = 0; x < kTexels; ++x)
                    EXPECT_EQ(scene.Pixel(x * block + block / 2, y * block + block / 2), texel(x, y));
        }

        // Every table page the two frames used came back once the GPU finished with it.
        device.WaitForIdle();
        EXPECT_EQ(device.GetDescriptorTables()->shaderResources.GetFreePageCount(),
                  size_t(Spark::RHI::D3D12::SRV_TABLE_PAGE_COUNT));
        EXPECT_EQ(device.GetDescriptorTables()->samplers.GetFreePageCount(),
                  size_t(Spark::RHI::D3D12::SAMPLER_TABLE_PAGE_COUNT));
        EXPECT_TRUE(SUCCEEDED(device.GetD3D12Device()->GetDeviceRemovedReason()));
        ExpectClean(device);
    }
    device.Shutdown();
}

TEST(D3D12_Validation_ResourceChurnIsClean)
{
    D3D12Device device;
    InitializeValidated(device);

    constexpr int kCycles = 200;
    for (int cycle = 0; cycle < kCycles; ++cycle)
    {
        device.BeginFrame();

        RHIBufferDesc dynamic;
        dynamic.size = 256;
        dynamic.stride = 16;
        dynamic.usage = RHIBufferUsage::Vertex;
        dynamic.access = RHIBufferAccess::Dynamic;
        dynamic.debugName = "RHI225ChurnDynamic";
        auto dynamicBuffer = device.CreateBuffer(dynamic);

        RHIBufferDesc gpuOnly = dynamic;
        gpuOnly.access = RHIBufferAccess::Static;
        gpuOnly.debugName = "RHI225ChurnStatic";
        auto staticBuffer = device.CreateBuffer(gpuOnly);

        RHITextureDesc color;
        color.width = 16;
        color.height = 16;
        color.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
        color.debugName = "RHI225ChurnTarget";
        auto target = device.CreateTexture(color);
        ASSERT_TRUE(dynamicBuffer != nullptr && staticBuffer != nullptr && target != nullptr);

        // Use the texture on the GPU, then destroy everything while that work is in flight:
        // the fence-deferred release queue must keep it alive until the GPU is done.
        auto* cmd = static_cast<D3D12CommandList*>(device.GetImmediateCommandList());
        auto* native = static_cast<D3D12Texture*>(target.get());
        cmd->Begin();
        cmd->TransitionBarrier(native, native->GetCurrentState(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->FlushBarriers();
        const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        cmd->ClearRenderTarget(target.get(), clear);
        cmd->TransitionBarrier(native, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        cmd->End();
        device.ExecuteCommandList(cmd);

        dynamicBuffer.reset();
        staticBuffer.reset();
        target.reset();

        // The immediate list owns one allocator, so it must be idle before its next Begin().
        device.WaitForIdle();
        device.EndFrame();
    }

    device.WaitForIdle();
    device.BeginFrame(); // processes the releases whose fence has passed
    device.EndFrame();
    EXPECT_EQ(device.GetPendingReleaseCount(), size_t(0));
    EXPECT_TRUE(SUCCEEDED(device.GetD3D12Device()->GetDeviceRemovedReason()));
    ExpectClean(device);
    device.Shutdown();
}

TEST(D3D12_Validation_CounterSeesInjectedError)
{
    D3D12Device device;
    InitializeValidated(device);
    const D3D12ValidationCounts before = device.GetValidationCounts();

    // A buffer must have Height 1; the runtime rejects this and the debug layer reports an error.
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC invalid = {};
    invalid.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    invalid.Width = 256;
    invalid.Height = 2;
    invalid.DepthOrArraySize = 1;
    invalid.MipLevels = 1;
    invalid.SampleDesc.Count = 1;
    invalid.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> resource;
    const HRESULT result = device.GetD3D12Device()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &invalid, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource));
    EXPECT_TRUE(FAILED(result));

    // The process survived (no unconditional break-on-error) and the counter saw it.
    const D3D12ValidationCounts after = device.GetValidationCounts();
    EXPECT_TRUE(after.errors + after.corruption > before.errors + before.corruption);
    device.Shutdown();
}

#endif // _WIN32 && !SPARK_NO_D3D12
