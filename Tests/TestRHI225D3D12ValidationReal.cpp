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

#include <cstdint>
#include <cstdio>
#include <cstring>
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

    /// Solid-colour triangle pipeline, vertex buffer and a colour target with a READBACK copy.
    struct TriangleScene
    {
        std::unique_ptr<IRHIShader> vertexShader;
        std::unique_ptr<IRHIShader> pixelShader;
        std::unique_ptr<IRHIPipelineState> pipeline;
        std::unique_ptr<IRHIBuffer> vertices;
        std::unique_ptr<IRHITexture> target;
        ComPtr<ID3D12Resource> readback;

        explicit TriangleScene(D3D12Device& device)
        {
            RHIShaderDesc vs;
            vs.stage = RHIShaderStage::Vertex;
            vs.sourceCode = kVertexShader;
            vs.filePath = "RHI225TriangleVS";
            vertexShader = device.CreateShader(vs);
            RHIShaderDesc ps;
            ps.stage = RHIShaderStage::Pixel;
            ps.sourceCode = kPixelShader;
            ps.filePath = "RHI225TrianglePS";
            pixelShader = device.CreateShader(ps);
            ASSERT_TRUE(vertexShader != nullptr && pixelShader != nullptr);

            RHIPipelineStateDesc pso;
            RHIInputElement position;
            position.semanticName = "POSITION";
            position.format = RHIVertexFormat::Float3;
            pso.inputLayout.elements.push_back(position);
            pso.rasterizer.cullMode = RHICullMode::None;
            pso.depthStencil.depthEnable = false;
            pso.depthStencil.depthWrite = false;
            pso.depthStencilFormat = PixelFormat::Unknown;
            pso.numRenderTargets = 1;
            pso.renderTargetFormats[0] = PixelFormat::R8G8B8A8_UNORM;
            pipeline = device.CreatePipelineState(pso, vertexShader.get(), pixelShader.get());
            ASSERT_TRUE(pipeline != nullptr);

            // Covers the centre pixel, leaves the corners at the clear colour.
            const float triangle[9] = {-0.5f, -0.5f, 0.0f, 0.0f, 0.5f, 0.0f, 0.5f, -0.5f, 0.0f};
            RHIBufferDesc vb;
            vb.size = sizeof(triangle);
            vb.stride = 3 * sizeof(float);
            vb.usage = RHIBufferUsage::Vertex;
            vb.access = RHIBufferAccess::Dynamic;
            vb.initialData = triangle;
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
        void RenderFrame(D3D12Device& device)
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
            cmd->Draw(3, 0);

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
