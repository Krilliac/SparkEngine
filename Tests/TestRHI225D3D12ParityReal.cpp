/**
 * @file TestRHI225D3D12ParityReal.cpp
 * @brief RHI-225: the declared D3D11/D3D12 parity matrix.
 *
 * Every scene in kScenes renders the same HLSL through D3D11Device and D3D12Device, driven
 * only through IRHIDevice/IRHICommandList (readback is the one backend-specific step). Each
 * scene records two frames, the second into a reset list, and compares the second:
 *
 *   - Cross-backend: a pixel differs when any channel is more than kChannelTolerance apart;
 *     at most kMaxDifferingFraction of the pixels may differ.
 *   - Analytic: each backend's frame must match the scene's CPU expectation (don't-care only
 *     within a pixel of a triangle edge or a bloom threshold), so two equally broken
 *     backends cannot pass together, and must pass GoldenImageTestRunner::FrameHasRenderedContent.
 *
 * These are cross-backend checks, not goldens: no baseline images or reviewed thresholds.
 * A scene whose D3D12 feature is missing fails; nothing skips. Both devices must run on the
 * same adapter (equal LUIDs, or both software), so a cross-GPU comparison cannot hide or
 * invent a difference. The scene table is mirrored in wiki/graphics/D3D12-Backend.md between
 * the parity-matrix markers, and every test checks the two list the same scenes in order.
 * Scope: RHI level only; GraphicsEngine renders through D3D11 directly on Windows.
 */

#include "TestFramework.h"
#include "Utils/GoldenImageTest.h"

#if defined(_WIN32) && !defined(SPARK_NO_D3D12)

#include "Graphics/RHI/D3D11/D3D11Device.h"
#include "Graphics/RHI/D3D12/D3D12Device.h"
#include "Graphics/RHI/RHIPipelineTypes.h"
#include "Graphics/RHI/RHIResources.h"
#include "Graphics/RHI/RHITypes.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using Microsoft::WRL::ComPtr;
    using namespace Spark::RHI;

    constexpr uint32_t kSize = 64;
    constexpr uint32_t kRowPitch = kSize * 4; // 256: already D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
    constexpr int kChannelTolerance = 2;
    constexpr double kMaxDifferingFraction = 0.005;
    constexpr float kBlack[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    struct Rgba
    {
        uint8_t r = 0, g = 0, b = 0, a = 255;
    };
    using Expectation = std::optional<Rgba>;

    // ========================================================================
    // Backends
    // ========================================================================

    struct AdapterIdentity
    {
        LUID luid = {};
        bool software = false;
        std::string description;
    };

    std::string Narrow(const wchar_t* text)
    {
        char buffer[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, sizeof(buffer) - 1, nullptr, nullptr);
        return buffer;
    }

    AdapterIdentity Identify(IDXGIAdapter* adapter)
    {
        AdapterIdentity identity;
        ComPtr<IDXGIAdapter1> adapter1;
        DXGI_ADAPTER_DESC1 desc = {};
        if (adapter && SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter1))) &&
            SUCCEEDED(adapter1->GetDesc1(&desc)))
        {
            identity.luid = desc.AdapterLuid;
            identity.software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            identity.description = Narrow(desc.Description);
        }
        return identity;
    }

    /// One backend under test: its device, frame submission and RGBA8 readback.
    class ParityBackend
    {
      public:
        virtual ~ParityBackend() = default;
        virtual IRHIDevice& Device() = 0;
        virtual AdapterIdentity Adapter() = 0;
        virtual IRHICommandList& BeginFrame() = 0;
        /// Submits the frame, waits for the GPU and returns each target's rows, concatenated.
        virtual std::vector<uint8_t> EndFrame(const std::vector<IRHITexture*>& targets) = 0;
    };

    class D3D11Backend final : public ParityBackend
    {
      public:
        bool Open()
        {
            RHIDeviceDesc desc;
            desc.preferredBackend = GraphicsBackend::D3D11;
            desc.applicationName = "SparkTests_RHI225_Parity_D3D11";
            return m_device.Initialize(desc);
        }

        IRHIDevice& Device() override { return m_device; }

        AdapterIdentity Adapter() override
        {
            ComPtr<IDXGIDevice> dxgiDevice;
            ComPtr<IDXGIAdapter> adapter;
            if (FAILED(m_device.GetD3D11Device()->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) ||
                FAILED(dxgiDevice->GetAdapter(&adapter)))
                return {};
            return Identify(adapter.Get());
        }

        IRHICommandList& BeginFrame() override
        {
            m_device.BeginFrame();
            IRHICommandList* cmd = m_device.GetImmediateCommandList();
            cmd->Begin();
            return *cmd;
        }

        std::vector<uint8_t> EndFrame(const std::vector<IRHITexture*>& targets) override
        {
            IRHICommandList* cmd = m_device.GetImmediateCommandList();
            cmd->End();
            m_device.ExecuteCommandList(cmd);
            std::vector<uint8_t> pixels;
            for (IRHITexture* target : targets)
                AppendReadback(static_cast<ID3D11Resource*>(target->GetNativeHandle()), pixels);
            m_device.EndFrame();
            return pixels;
        }

      private:
        /// Copies a kSize x kSize RGBA8 texture through a staging copy and appends its rows.
        void AppendReadback(ID3D11Resource* resource, std::vector<uint8_t>& pixels)
        {
            D3D11_TEXTURE2D_DESC stagingDesc = {};
            stagingDesc.Width = kSize;
            stagingDesc.Height = kSize;
            stagingDesc.MipLevels = 1;
            stagingDesc.ArraySize = 1;
            stagingDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            stagingDesc.SampleDesc.Count = 1;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            ASSERT_TRUE(SUCCEEDED(m_device.GetD3D11Device()->CreateTexture2D(&stagingDesc, nullptr, &staging)));

            ID3D11DeviceContext1* context = m_device.GetD3D11Context();
            context->CopyResource(staging.Get(), resource);
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            ASSERT_TRUE(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
            for (uint32_t y = 0; y < kSize; ++y)
            {
                const auto* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
                pixels.insert(pixels.end(), row, row + kRowPitch);
            }
            context->Unmap(staging.Get(), 0);
        }

        D3D11::D3D11Device m_device;
    };

    class D3D12Backend final : public ParityBackend
    {
      public:
        bool Open()
        {
            RHIDeviceDesc desc;
            desc.applicationName = "SparkTests_RHI225_Parity_D3D12";
            return m_device.Initialize(desc);
        }

        IRHIDevice& Device() override { return m_device; }

        AdapterIdentity Adapter() override
        {
            AdapterIdentity identity = Identify(m_device.GetAdapter());
            identity.luid = m_device.GetD3D12Device()->GetAdapterLuid();
            return identity;
        }

        IRHICommandList& BeginFrame() override
        {
            m_device.BeginFrame();
            IRHICommandList* cmd = m_device.GetImmediateCommandList();
            cmd->Begin();
            return *cmd;
        }

        std::vector<uint8_t> EndFrame(const std::vector<IRHITexture*>& targets) override
        {
            auto* cmd = static_cast<D3D12::D3D12CommandList*>(m_device.GetImmediateCommandList());
            while (m_readback.size() < targets.size())
                m_readback.push_back(CreateReadbackBuffer());
            for (size_t i = 0; i < targets.size(); ++i)
            {
                auto* texture = static_cast<D3D12::D3D12Texture*>(targets[i]);
                cmd->RequireState(texture, D3D12_RESOURCE_STATE_COPY_SOURCE);
                cmd->FlushBarriers();
                D3D12_TEXTURE_COPY_LOCATION source = {};
                source.pResource = texture->GetD3D12Resource();
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION destination = {};
                destination.pResource = m_readback[i].Get();
                destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                destination.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                destination.PlacedFootprint.Footprint.Width = kSize;
                destination.PlacedFootprint.Footprint.Height = kSize;
                destination.PlacedFootprint.Footprint.Depth = 1;
                destination.PlacedFootprint.Footprint.RowPitch = kRowPitch;
                cmd->GetCommandList()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            }
            cmd->End();
            m_device.ExecuteCommandList(cmd);
            m_device.WaitForIdle();
            m_device.EndFrame();

            std::vector<uint8_t> pixels;
            for (size_t i = 0; i < targets.size(); ++i)
            {
                const D3D12_RANGE readRange = {0, static_cast<SIZE_T>(kRowPitch) * kSize};
                void* mapped = nullptr;
                ASSERT_TRUE(SUCCEEDED(m_readback[i]->Map(0, &readRange, &mapped)) && mapped != nullptr);
                const auto* bytes = static_cast<const uint8_t*>(mapped);
                pixels.insert(pixels.end(), bytes, bytes + size_t(kRowPitch) * kSize);
                const D3D12_RANGE noWrite = {0, 0};
                m_readback[i]->Unmap(0, &noWrite);
            }
            return pixels;
        }

      private:
        ComPtr<ID3D12Resource> CreateReadbackBuffer()
        {
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
            ComPtr<ID3D12Resource> readback;
            ASSERT_TRUE(SUCCEEDED(m_device.GetD3D12Device()->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&readback))));
            return readback;
        }

        D3D12::D3D12Device m_device;
        std::vector<ComPtr<ID3D12Resource>> m_readback;
    };

    // ========================================================================
    // Shared HLSL (compiled vs/ps_5_0 by D3D11, vs/ps_5_1 by D3D12)
    // ========================================================================

    constexpr const char* kPositionVS = R"(
float4 main(float3 position : POSITION) : SV_Position { return float4(position, 1.0); }
)";

    constexpr const char* kColorVS = R"(
struct Output { float4 color : COLOR; float4 position : SV_Position; };
Output main(float3 position : POSITION, float4 color : COLOR)
{
    Output output;
    output.color = color;
    output.position = float4(position, 1.0);
    return output;
}
)";

    // TEXCOORD first: the shipped bloom shader's input signature is TEXCOORD alone.
    constexpr const char* kUvVS = R"(
struct Output { float2 uv : TEXCOORD; float4 position : SV_Position; };
Output main(float3 position : POSITION, float2 uv : TEXCOORD)
{
    Output output;
    output.uv = uv;
    output.position = float4(position, 1.0);
    return output;
}
)";

    constexpr const char* kInstancedVS = R"(
struct Output { float4 color : COLOR; float4 position : SV_Position; };
Output main(float3 position : POSITION, float2 offset : INSTANCEOFFSET, float4 color : INSTANCECOLOR)
{
    Output output;
    output.color = color;
    output.position = float4(position.xy + offset, position.z, 1.0);
    return output;
}
)";

    constexpr const char* kRedPS = R"(
float4 main() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }
)";

    constexpr const char* kColorPS = R"(
float4 main(float4 color : COLOR) : SV_Target { return color; }
)";

    constexpr const char* kConstantPS = R"(
cbuffer Material : register(b0) { float4 materialColor; };
float4 main() : SV_Target { return materialColor; }
)";

    constexpr const char* kSamplePS = R"(
Texture2D source : register(t0);
SamplerState sourceSampler : register(s0);
float4 main(float2 uv : TEXCOORD) : SV_Target { return source.Sample(sourceSampler, uv); }
)";

    constexpr const char* kInvertSamplePS = R"(
Texture2D source : register(t0);
SamplerState sourceSampler : register(s0);
float4 main(float2 uv : TEXCOORD) : SV_Target { return float4(1.0 - source.Sample(sourceSampler, uv).rgb, 1.0); }
)";

    constexpr const char* kMrtPS = R"(
struct Targets { float4 first : SV_Target0; float4 second : SV_Target1; };
Targets main()
{
    Targets targets;
    targets.first = float4(1.0, 1.0, 0.0, 1.0);
    targets.second = float4(0.0, 1.0, 1.0, 1.0);
    return targets;
}
)";

    // ========================================================================
    // Resource helpers (backend-agnostic)
    // ========================================================================

    struct Pipeline
    {
        std::unique_ptr<IRHIShader> vertexShader;
        std::unique_ptr<IRHIShader> pixelShader;
        std::unique_ptr<IRHIPipelineState> state;
    };

    RHIInputElement Element(const char* semantic, RHIVertexFormat format, uint32_t offset, uint32_t slot = 0,
                            bool perInstance = false)
    {
        RHIInputElement element;
        element.semanticName = semantic;
        element.format = format;
        element.byteOffset = offset;
        element.inputSlot = slot;
        element.perInstance = perInstance;
        element.instanceStepRate = perInstance ? 1u : 0u;
        return element;
    }

    Pipeline MakePipeline(IRHIDevice& device, const std::string& vs, const std::string& ps,
                          std::vector<RHIInputElement> layout,
                          const std::function<void(RHIPipelineStateDesc&)>& customize = {},
                          const char* psEntry = "main")
    {
        Pipeline pipeline;
        RHIShaderDesc vertex;
        vertex.stage = RHIShaderStage::Vertex;
        vertex.sourceCode = vs;
        vertex.filePath = "RHI225ParityVS";
        vertex.debugName = "RHI225ParityVS"; // D3D11 names the compile after debugName; empty fails silently
        pipeline.vertexShader = device.CreateShader(vertex);
        RHIShaderDesc pixel;
        pixel.stage = RHIShaderStage::Pixel;
        pixel.sourceCode = ps;
        pixel.entryPoint = psEntry;
        pixel.filePath = "RHI225ParityPS";
        pixel.debugName = "RHI225ParityPS";
        pipeline.pixelShader = device.CreateShader(pixel);
        ASSERT_TRUE(pipeline.vertexShader != nullptr && pipeline.pixelShader != nullptr);

        RHIPipelineStateDesc desc;
        desc.inputLayout.elements = std::move(layout);
        desc.rasterizer.cullMode = RHICullMode::None;
        desc.depthStencil.depthEnable = false;
        desc.depthStencil.depthWrite = false;
        desc.depthStencilFormat = PixelFormat::Unknown;
        desc.numRenderTargets = 1;
        desc.renderTargetFormats[0] = PixelFormat::R8G8B8A8_UNORM;
        if (customize)
            customize(desc);
        pipeline.state = device.CreatePipelineState(desc, pipeline.vertexShader.get(), pipeline.pixelShader.get());
        ASSERT_TRUE(pipeline.state != nullptr);
        return pipeline;
    }

    std::unique_ptr<IRHIBuffer> MakeBuffer(IRHIDevice& device, RHIBufferUsage usage, RHIBufferAccess access,
                                           const void* data, size_t size, uint32_t stride)
    {
        RHIBufferDesc desc;
        desc.size = size;
        desc.stride = stride;
        desc.usage = usage;
        desc.access = access;
        desc.initialData = data;
        desc.debugName = "RHI225ParityBuffer";
        auto buffer = device.CreateBuffer(desc);
        ASSERT_TRUE(buffer != nullptr);
        return buffer;
    }

    /// A 256-byte constant buffer (one CBV block) whose first four floats are @p values.
    std::unique_ptr<IRHIBuffer> MakeConstants(IRHIDevice& device, std::array<float, 4> values)
    {
        std::array<float, 64> block = {};
        std::copy(values.begin(), values.end(), block.begin());
        return MakeBuffer(device, RHIBufferUsage::Constant, RHIBufferAccess::Dynamic, block.data(), sizeof(block), 0);
    }

    std::unique_ptr<IRHITexture> MakeTexture(IRHIDevice& device, uint32_t width, uint32_t height, PixelFormat format,
                                             RHITextureUsage usage)
    {
        RHITextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = format;
        desc.usage = usage;
        desc.debugName = "RHI225ParityTexture";
        auto texture = device.CreateTexture(desc);
        ASSERT_TRUE(texture != nullptr);
        return texture;
    }

    std::unique_ptr<IRHITexture> MakeTarget(IRHIDevice& device)
    {
        return MakeTexture(device, kSize, kSize, PixelFormat::R8G8B8A8_UNORM,
                           RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource);
    }

    std::unique_ptr<IRHISampler> MakeSampler(IRHIDevice& device, RHIFilterMode filter)
    {
        RHISamplerDesc desc;
        desc.minFilter = filter;
        desc.magFilter = filter;
        desc.mipFilter = filter;
        desc.addressU = RHIAddressMode::Clamp;
        desc.addressV = RHIAddressMode::Clamp;
        desc.addressW = RHIAddressMode::Clamp;
        auto sampler = device.CreateSampler(desc);
        ASSERT_TRUE(sampler != nullptr);
        return sampler;
    }

    /// Binds the targets, clears them to @p clear (depth to 1) and sets a full-target viewport
    /// and scissor (D3D12 always scissor-tests).
    void BeginPass(IRHICommandList& cmd, std::vector<IRHITexture*> targets, IRHITexture* depth, const float clear[4])
    {
        cmd.SetRenderTargets(targets.data(), static_cast<uint32_t>(targets.size()), depth);
        for (IRHITexture* target : targets)
            cmd.ClearRenderTarget(target, clear);
        if (depth)
            cmd.ClearDepthStencil(depth, 1.0f, 0);
        RHIViewport viewport;
        viewport.width = static_cast<float>(kSize);
        viewport.height = static_cast<float>(kSize);
        cmd.SetViewport(viewport);
        RHIScissorRect scissor;
        scissor.right = static_cast<int32_t>(kSize);
        scissor.bottom = static_cast<int32_t>(kSize);
        cmd.SetScissorRect(scissor);
    }

    void DrawWith(IRHICommandList& cmd, const Pipeline& pipeline, IRHIBuffer* vertices, uint32_t vertexCount)
    {
        cmd.SetPipelineState(pipeline.state.get());
        cmd.SetPrimitiveTopology(RHIPrimitiveTopology::TriangleList);
        cmd.SetVertexBuffer(vertices, 0, 0);
        cmd.Draw(vertexCount, 0);
    }

    /// Records @p record twice, the second time into a reset list that re-binds everything,
    /// and returns the second frame's readback of @p targets.
    std::vector<uint8_t> RenderFrames(ParityBackend& backend, const std::vector<IRHITexture*>& targets,
                                      const std::function<void(IRHICommandList&)>& record)
    {
        std::vector<uint8_t> frame;
        for (int pass = 0; pass < 2; ++pass)
        {
            IRHICommandList& cmd = backend.BeginFrame();
            record(cmd);
            frame = backend.EndFrame(targets);
        }
        return frame;
    }

    // ========================================================================
    // Scene geometry and CPU expectations
    // ========================================================================

    constexpr float kTriangle[6] = {-0.5f, -0.5f, 0.0f, 0.5f, 0.5f, -0.5f};      // x, y per vertex
    constexpr float kColorTriangle[6] = {-0.9f, -0.9f, 0.0f, 0.9f, 0.9f, -0.9f}; // red, green, blue
    constexpr Rgba kTriangleColors[3] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}};

    /// Barycentric weights of pixel (x, y)'s centre in an NDC triangle; @p nearEdge within a pixel of an edge.
    std::array<float, 3> Barycentric(const float (&ndc)[6], uint32_t x, uint32_t y, bool& nearEdge)
    {
        float px[3], py[3];
        for (int i = 0; i < 3; ++i)
        {
            px[i] = (ndc[i * 2] + 1.0f) * 0.5f * kSize;
            py[i] = (1.0f - ndc[i * 2 + 1]) * 0.5f * kSize;
        }
        const float cx = x + 0.5f;
        const float cy = y + 0.5f;
        const float area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
        std::array<float, 3> weights = {
            ((px[1] - cx) * (py[2] - cy) - (px[2] - cx) * (py[1] - cy)) / area,
            ((px[2] - cx) * (py[0] - cy) - (px[0] - cx) * (py[2] - cy)) / area,
            0.0f,
        };
        weights[2] = 1.0f - weights[0] - weights[1];
        nearEdge = false;
        for (int i = 0; i < 3; ++i)
        {
            // Distance to the edge opposite vertex i = weight x altitude.
            const float edgeX = px[(i + 2) % 3] - px[(i + 1) % 3];
            const float edgeY = py[(i + 2) % 3] - py[(i + 1) % 3];
            const float altitude = std::abs(area) / std::sqrt(edgeX * edgeX + edgeY * edgeY);
            if (std::abs(weights[i] * altitude) < 1.0f)
                nearEdge = true;
        }
        return weights;
    }

    bool Inside(const std::array<float, 3>& weights)
    {
        return weights[0] > 0.0f && weights[1] > 0.0f && weights[2] > 0.0f;
    }

    /// Pixel centre inside the rectangle [left, right) x [top, bottom) in pixels.
    bool InRect(uint32_t x, uint32_t y, float left, float top, float right, float bottom)
    {
        const float cx = x + 0.5f;
        const float cy = y + 0.5f;
        return cx > left && cx < right && cy > top && cy < bottom;
    }

    uint8_t Unorm(float value)
    {
        return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    }

    Expectation ColorTriangleAt(uint32_t x, uint32_t y, Rgba outside)
    {
        bool nearEdge = false;
        const auto weights = Barycentric(kColorTriangle, x, y, nearEdge);
        if (nearEdge)
            return std::nullopt;
        if (!Inside(weights))
            return outside;
        Rgba color;
        color.r = Unorm(weights[0]);
        color.g = Unorm(weights[1]);
        color.b = Unorm(weights[2]);
        return color;
    }

    // 4x4 point-sampled texture: every texel distinct.
    constexpr uint32_t kPointTexels = 4;
    Rgba PointTexel(uint32_t x, uint32_t y)
    {
        return Rgba{static_cast<uint8_t>(40 + 50 * x), static_cast<uint8_t>(40 + 50 * y),
                    static_cast<uint8_t>((x + y) * 20), 255};
    }

    // 2x2 bilinear texture: red, green / blue, white.
    constexpr Rgba kLinearTexels[2][2] = {{{255, 0, 0, 255}, {0, 255, 0, 255}},
                                          {{0, 0, 255, 255}, {255, 255, 255, 255}}};

    // Shipped bloom input: 64x64 RGBA32F, R ramps past 1.0 (HDR), G ramps, B constant.
    std::array<float, 4> BloomInput(uint32_t x, uint32_t y)
    {
        return {x / 63.0f * 1.5f, y / 63.0f, 0.25f, 1.0f};
    }
    constexpr float kBloomThreshold = 0.5f;

    // Instances: offset (NDC) and colour, one per quadrant.
    constexpr float kInstances[4][6] = {{-0.5f, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f},
                                        {0.5f, 0.5f, 0.0f, 1.0f, 0.0f, 1.0f},
                                        {-0.5f, -0.5f, 0.0f, 0.0f, 1.0f, 1.0f},
                                        {0.5f, -0.5f, 1.0f, 1.0f, 1.0f, 1.0f}};

    /// A full-target quad (two triangles) with UVs, (0,0) at the top-left.
    constexpr float kFullQuadUv[30] = {-1.0f, 1.0f,  0.0f, 0.0f, 0.0f, 1.0f,  1.0f,  0.0f, 1.0f, 0.0f,
                                       -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, -1.0f, 0.0f, 0.0f, 1.0f,
                                       1.0f,  1.0f,  0.0f, 1.0f, 0.0f, 1.0f,  -1.0f, 0.0f, 1.0f, 1.0f};

    /// Two triangles covering the NDC rectangle at depth @p z, as float3 positions.
    std::array<float, 18> QuadPositions(float left, float bottom, float right, float top, float z)
    {
        return {left, top, z, right, top, z, left, bottom, z, left, bottom, z, right, top, z, right, bottom, z};
    }

    /// Position + colour vertices (7 floats each) for an NDC rectangle.
    std::vector<float> ColoredQuad(float left, float bottom, float right, float top, float z, Rgba color, float alpha)
    {
        const auto positions = QuadPositions(left, bottom, right, top, z);
        std::vector<float> vertices;
        for (size_t i = 0; i < positions.size(); i += 3)
        {
            vertices.insert(vertices.end(), {positions[i], positions[i + 1], positions[i + 2], color.r / 255.0f,
                                             color.g / 255.0f, color.b / 255.0f, alpha});
        }
        return vertices;
    }

    std::vector<RHIInputElement> PositionLayout()
    {
        return {Element("POSITION", RHIVertexFormat::Float3, 0)};
    }
    std::vector<RHIInputElement> ColorLayout()
    {
        return {Element("POSITION", RHIVertexFormat::Float3, 0), Element("COLOR", RHIVertexFormat::Float4, 12)};
    }
    std::vector<RHIInputElement> UvLayout()
    {
        return {Element("POSITION", RHIVertexFormat::Float3, 0), Element("TEXCOORD", RHIVertexFormat::Float2, 12)};
    }

    // ========================================================================
    // Scenes: render (records two frames, returns the second) + CPU expectation
    // ========================================================================

    std::vector<uint8_t> RenderSolidTriangle(ParityBackend& backend)
    {
        IRHIDevice& device = backend.Device();
        const Pipeline pipeline = MakePipeline(device, kPositionVS, kRedPS, PositionLayout());
        const float vertices[9] = {kTriangle[0], kTriangle[1], 0.0f,         kTriangle[2], kTriangle[3],
                                   0.0f,         kTriangle[4], kTriangle[5], 0.0f};
        auto vb = MakeBuffer(device, RHIBufferUsage::Vertex, RHIBufferAccess::Dynamic, vertices, sizeof(vertices), 12);
        auto target = MakeTarget(device);
        return RenderFrames(backend, {target.get()},
                            [&](IRHICommandList& cmd)
                            {
                                BeginPass(cmd, {target.get()}, nullptr, kBlack);
                                DrawWith(cmd, pipeline, vb.get(), 3);
                            });
    }
    Expectation ExpectSolidTriangle(uint32_t x, uint32_t y)
    {
        bool nearEdge = false;
        const auto weights = Barycentric(kTriangle, x, y, nearEdge);
        if (nearEdge)
            return std::nullopt;
        return Inside(weights) ? Rgba{255, 0, 0, 255} : Rgba{0, 0, 0, 255};
    }

    std::vector<uint8_t> RenderVertexColorInterpolation(ParityBackend& backend)
    {
        IRHIDevice& device = backend.Device();
        const Pipeline pipeline = MakePipeline(device, kColorVS, kColorPS, ColorLayout());
        std::vector<float> vertices;
        for (int i = 0; i < 3; ++i)
        {
            const Rgba c = kTriangleColors[i];
            vertices.insert(vertices.end(), {kColorTriangle[i * 2], kColorTriangle[i * 2 + 1], 0.0f, c.r / 255.0f,
                                             c.g / 255.0f, c.b / 255.0f, 1.0f});
        }
        auto vb = MakeBuffer(device, RHIBufferUsage::Vertex, RHIBufferAccess::Dynamic, vertices.data(),
                             vertices.size() * sizeof(float), 28);
        auto target = MakeTarget(device);
        return RenderFrames(backend, {target.get()},
                            [&](IRHICommandList& cmd)
                            {
                                BeginPass(cmd, {target.get()}, nullptr, kBlack);
                                DrawWith(cmd, pipeline, vb.get(), 3);
                            });
    }
    Expectation ExpectVertexColorInterpolation(uint32_t x, uint32_t y)
    {
        return ColorTriangleAt(x, y, Rgba{0, 0, 0, 255});
    }

    std::vector<uint8_t> RenderConstantBufferColor(ParityBackend& backend)
    {
        IRHIDevice& device = backend.Device();
        const Pipeline pipeline = MakePipeline(device, kPositionVS, kConstantPS, PositionLayout());
        const auto quad = QuadPositions(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f);
        auto vb = MakeBuffer(device, RHIBufferUsage::Vertex, RHIBufferAccess::Dynamic, quad.data(), sizeof(quad), 12);
        auto material = MakeConstants(device, {51.0f / 255.0f, 102.0f / 255.0f, 204.0f / 255.0f, 1.0f});
        auto target = MakeTarget(device);
        return RenderFrames(backend, {target.get()},
                            [&](IRHICommandList& cmd)
                            {
                                BeginPass(cmd, {target.get()}, nullptr, kBlack);
                                cmd.SetConstantBuffer(RHIShaderStage::Pixel, 0, material.get());
                                DrawWith(cmd, pipeline, vb.get(), 6);
                            });
    }
    Expectation ExpectConstantBufferColor(uint32_t x, uint32_t y)
    {
        return InRect(x, y, 16, 16, 48, 48) ? Rgba{51, 102, 204, 255} : Rgba{0, 0, 0, 255};
    }

    /// Full-target quad sampling @p texture through @p filter.
    std::vector<uint8_t> RenderSampledQuad(ParityBackend& backend, IRHITexture* texture, RHIFilterMode filter)
    {
        IRHIDevice& device = backend.Device();
        const Pipeline pipeline = MakePipeline(device, kUvVS, kSamplePS, UvLayout());
        auto vb =
            MakeBuffer(device, RHIBufferUsage::Vertex, RHIBufferAccess::Dynamic, kFullQuadUv, sizeof(kFullQuadUv), 20);
        auto sampler = MakeSampler(device, filter);
        auto target = MakeTarget(device);
        return RenderFrames(backend, {target.get()},
                            [&](IRHICommandList& cmd)
                            {
                                BeginPass(cmd, {target.get()}, nullptr, kBlack);
                                cmd.SetShaderResource(RHIShaderStage::Pixel, 0, texture);
                                cmd.SetSampler(RHIShaderStage::Pixel, 0, sampler.get());
                                DrawWith(cmd, pipeline, vb.get(), 6);
                            });
    }

    std::vector<uint8_t> RenderTexturedQuadPoint(ParityBackend& backend)
    {
        std::vector<Rgba> texels;
        for (uint32_t y = 0; y < kPointTexels; ++y)
            for (uint32_t x = 0; x < kPointTexels; ++x)
                texels.push_back(PointTexel(x, y));
        auto texture = MakeTexture(backend.Device(), kPointTexels, kPointTexels, PixelFormat::R8G8B8A8_UNORM,
                                   RHITextureUsage::ShaderResource);
        backend.Device().UpdateTexture(texture.get(), texels.data());
        return RenderSampledQuad(backend, texture.get(), RHIFilterMode::Nearest);
    }
    Expectation ExpectTexturedQuadPoint(uint32_t x, uint32_t y)
    {
        return PointTexel(x / (kSize / kPointTexels), y / (kSize / kPointTexels));
    }

    std::vector<uint8_t> RenderTexturedQuadLinear(ParityBackend& backend)
    {
        const Rgba texels[4] = {kLinearTexels[0][0], kLinearTexels[0][1], kLinearTexels[1][0], kLinearTexels[1][1]};
        auto texture =
            MakeTexture(backend.Device(), 2, 2, PixelFormat::R8G8B8A8_UNORM, RHITextureUsage::ShaderResource);
        backend.Device().UpdateTexture(texture.get(), texels);
        return RenderSampledQuad(backend, texture.get(), RHIFilterMode::Linear);
    }
    Expectation ExpectTexturedQuadLinear(uint32_t x, uint32_t y)
    {
        // Texel centres sit at 0.25 and 0.75; clamp addressing holds the edge texel beyond them.
        const float fx = std::clamp((x + 0.5f) / kSize * 2.0f - 0.5f, 0.0f, 1.0f);
        const float fy = std::clamp((y + 0.5f) / kSize * 2.0f - 0.5f, 0.0f, 1.0f);
        const auto channel = [&](uint8_t Rgba::*member)
        {
            const float top = kLinearTexels[0][0].*member * (1 - fx) + kLinearTexels[0][1].*member * fx;
            const float bottom = kLinearTexels[1][0].*member * (1 - fx) + kLinearTexels[1][1].*member * fx;
            return static_cast<uint8_t>(std::lround(top * (1 - fy) + bottom * fy));
        };
        return Rgba{channel(&Rgba::r), channel(&Rgba::g), channel(&Rgba::b), 255};
    }

    std::vector<uint8_t> RenderDepthTestOrdering(ParityBackend& backend)
    {
        IRHIDevice& device = backend.Device();
        const Pipeline pipeline = MakePipeline(device, kColorVS, kColorPS, ColorLayout(),
                                               [](RHIPipelineStateDesc& desc)
                                               {
                                                   desc.depthStencil.depthEnable = true;
                                                   desc.depthStencil.depthWrite = true;
                                                   desc.depthStencil.depthFunc = RHICompareOp::Less;
                                                   desc.depthStencilFormat = PixelFormat::D32_FLOAT;
                                               });
        // Near green quad first, then a far red quad over it: only the depth test keeps the overlap green.
        std::vector<float> vertices = ColoredQuad(-0.75f, -0.5f, 0.25f, 0.5f, 0.2f, Rgba{0, 255, 0, 255}, 1.0f);
        const std::vector<float> farQuad = ColoredQuad(-0.25f, -0.5f, 0.75f, 0.5f, 0.8f, Rgba{255, 0, 0, 255}, 1.0f);
        vertices.insert(vertices.end(), farQuad.begin(), farQuad.end());
        auto vb = MakeBuffer(device, RHIBufferUsage::Vertex, RHIBufferAccess::Dynamic, vertices.data(),
                             vertices.size() * sizeof(float), 28);
        auto target = MakeTarget(device);
        auto depth = MakeTexture(device, kSize, kSize, PixelFormat::D32_FLOAT, RHITextureUsage::DepthStencil);
        return RenderFrames(backend, {target.get()},
                            [&](IRHICommandList& cmd)
                            {
                                BeginPass(cmd, {target.get()}, depth.get(), kBlack);
                                DrawWith(cmd, pipeline, vb.get(), 12);
                            });
    }
    Expectation ExpectDepthTestOrdering(uint32_t x, uint32_t y)
    {
        if (InRect(x, y, 8, 16, 40, 48))
            return Rgba{0, 255, 0, 255};
        if (InRect(x, y, 24, 16, 56, 48))
            return Rgba{255, 0, 0, 255};
        return Rgba{0, 0, 0, 255};
    }

    std::vector<uint8_t> RenderAlphaBlendOver(ParityBackend& backend)
    {
        IRHIDevice& device = backend.Device();
        const Pipeline pipeline = MakePipeline(device, kColorVS, kColorPS, ColorLayout(),
                                               [](RHIPipelineStateDesc& desc)
                                               {
                                                   RHIBlendTargetDesc& blend = desc.blend.renderTargets[0];
                                                   blend.blendEnable = true;
                                                   blend.srcBlend = RHIBlendFactor::SrcAlpha;
                                                   blend.dstBlend = RHIBlendFactor::InvSrcAlpha;
                                                   blend.srcBlendAlpha = RHIBlendFactor::One;
                                                   blend.dstBlendAlpha = RHIBlendFactor::Zero;
                                               });
        const std::vector<float> vertices = ColoredQuad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f, Rgba{255, 0, 0, 255}, 0.25f);
        auto vb = MakeBuffer(device, RHIBufferUsage::Vertex, RHIBufferAccess::Dynamic, vertices.data(),
                             vertices.size() * sizeof(float), 28);
        auto target = MakeTarget(device);
        const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        return RenderFrames(backend, {target.get()},
                            [&](IRHICommandList& cmd)
                            {
                                BeginPass(cmd, {target.get()}, nullptr, blue);
                                DrawWith(cmd, pipeline, vb.get(), 6);
                            });
    }
    Expectation ExpectAlphaBlendOver(uint32_t x, uint32_t y)
    {
        // 0.25 * red over blue: R 0.25, B 0.75, A = 0.25 (One/Zero on alpha).
        return InRect(x, y, 16, 16, 48, 48) ? Rgba{64, 0, 191, 64} : Rgba{0, 0, 255, 255};
    }

    std::vector<uint8_t> RenderIndexedInstanced(ParityBackend& backend)
    {
        IRHIDevice& device = backend.Device();
        const Pipeline pipeline = MakePipeline(device, kInstancedVS, kColorPS,
                                               {Element("POSITION", RHIVertexFormat::Float3, 0),
                                                Element("INSTANCEOFFSET", RHIVertexFormat::Float2, 0, 1, true),
                                                Element("INSTANCECOLOR", RHIVertexFormat::Float4, 8, 1, true)});
        // Static (DEFAULT-heap) vertex, index and instance buffers: the D3D12 path copies initial data on creation.
        const float corners[12] = {-0.25f, 0.25f, 0.0f, 0.25f, 0.25f, 0.0f, -0.25f, -0.25f, 0.0f, 0.25f, -0.25f, 0.0f};
        const uint16_t indices[6] = {0, 1, 2, 2, 1, 3};
        auto vb = MakeBuffer(device, RHIBufferUsage::Vertex, RHIBufferAccess::Static, corners, sizeof(corners), 12);
        auto ib = MakeBuffer(device, RHIBufferUsage::Index, RHIBufferAccess::Static, indices, sizeof(indices), 2);
        auto instances =
            MakeBuffer(device, RHIBufferUsage::Vertex, RHIBufferAccess::Static, kInstances, sizeof(kInstances), 24);
        auto target = MakeTarget(device);
        return RenderFrames(backend, {target.get()},
                            [&](IRHICommandList& cmd)
                            {
                                BeginPass(cmd, {target.get()}, nullptr, kBlack);
                                cmd.SetPipelineState(pipeline.state.get());
                                cmd.SetPrimitiveTopology(RHIPrimitiveTopology::TriangleList);
                                cmd.SetVertexBuffer(vb.get(), 0, 0);
                                cmd.SetVertexBuffer(instances.get(), 1, 0);
                                cmd.SetIndexBuffer(ib.get(), 0);
                                cmd.DrawIndexedInstanced(6, 4, 0, 0, 0);
                            });
    }
    Expectation ExpectIndexedInstanced(uint32_t x, uint32_t y)
    {
        for (const auto& instance : kInstances)
        {
            const float cx = (instance[0] + 1.0f) * 0.5f * kSize;
            const float cy = (1.0f - instance[1]) * 0.5f * kSize;
            if (InRect(x, y, cx - 8, cy - 8, cx + 8, cy + 8))
                return Rgba{Unorm(instance[2]), Unorm(instance[3]), Unorm(instance[4]), 255};
        }
        return Rgba{0, 0, 0, 255};
    }

    std::vector<uint8_t> RenderViewportScissor(ParityBackend& backend)
    {
        IRHIDevice& device = backend.Device();
        const Pipeline pipeline =
            MakePipeline(device, kPositionVS, kRedPS, PositionLayout(),
                         [](RHIPipelineStateDesc& desc) { desc.rasterizer.scissorEnable = true; });
        const auto quad = QuadPositions(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f);
        auto vb = MakeBuffer(device, RHIBufferUsage::Vertex, RHIBufferAccess::Dynamic, quad.data(), sizeof(quad), 12);
        auto target = MakeTarget(device);
        return RenderFrames(backend, {target.get()},
                            [&](IRHICommandList& cmd)
                            {
                                BeginPass(cmd, {target.get()}, nullptr, kBlack);
                                // Viewport: right half. Scissor: top half. Only the top-right quadrant survives.
                                RHIViewport viewport;
                                viewport.x = kSize / 2.0f;
                                viewport.width = kSize / 2.0f;
                                viewport.height = static_cast<float>(kSize);
                                cmd.SetViewport(viewport);
                                RHIScissorRect scissor;
                                scissor.right = static_cast<int32_t>(kSize);
                                scissor.bottom = static_cast<int32_t>(kSize / 2);
                                cmd.SetScissorRect(scissor);
                                DrawWith(cmd, pipeline, vb.get(), 6);
                            });
    }
    Expectation ExpectViewportScissor(uint32_t x, uint32_t y)
    {
        return (x >= kSize / 2 && y < kSize / 2) ? Rgba{255, 0, 0, 255} : Rgba{0, 0, 0, 255};
    }

    std::vector<uint8_t> RenderMRTClearAndDraw(ParityBackend& backend)
    {
        IRHIDevice& device = backend.Device();
        const Pipeline pipeline = MakePipeline(device, kPositionVS, kMrtPS, PositionLayout(),
                                               [](RHIPipelineStateDesc& desc)
                                               {
                                                   desc.numRenderTargets = 2;
                                                   desc.renderTargetFormats[1] = PixelFormat::R8G8B8A8_UNORM;
                                               });
        const auto quad = QuadPositions(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f);
        auto vb = MakeBuffer(device, RHIBufferUsage::Vertex, RHIBufferAccess::Dynamic, quad.data(), sizeof(quad), 12);
        auto first = MakeTarget(device);
        auto second = MakeTarget(device);
        const float firstClear[4] = {0.2f, 0.0f, 0.0f, 1.0f};
        const float secondClear[4] = {0.0f, 0.4f, 0.0f, 1.0f};
        return RenderFrames(backend, {first.get(), second.get()},
                            [&](IRHICommandList& cmd)
                            {
                                BeginPass(cmd, {first.get(), second.get()}, nullptr, firstClear);
                                cmd.ClearRenderTarget(second.get(), secondClear);
                                DrawWith(cmd, pipeline, vb.get(), 6);
                            });
    }
    Expectation ExpectMRTClearAndDraw(uint32_t x, uint32_t y)
    {
        const bool second = y >= kSize;
        const bool inside = InRect(x, y % kSize, 16, 16, 48, 48);
        if (second)
            return inside ? Rgba{0, 255, 255, 255} : Rgba{0, 102, 0, 255};
        return inside ? Rgba{255, 255, 0, 255} : Rgba{51, 0, 0, 255};
    }

    std::vector<uint8_t> RenderRenderToTextureThenSample(ParityBackend& backend)
    {
        IRHIDevice& device = backend.Device();
        const Pipeline colorPipeline = MakePipeline(device, kColorVS, kColorPS, ColorLayout());
        const Pipeline postPipeline = MakePipeline(device, kUvVS, kInvertSamplePS, UvLayout());
        std::vector<float> triangle;
        for (int i = 0; i < 3; ++i)
        {
            const Rgba c = kTriangleColors[i];
            triangle.insert(triangle.end(), {kColorTriangle[i * 2], kColorTriangle[i * 2 + 1], 0.0f, c.r / 255.0f,
                                             c.g / 255.0f, c.b / 255.0f, 1.0f});
        }
        auto triangleVb = MakeBuffer(device, RHIBufferUsage::Vertex, RHIBufferAccess::Dynamic, triangle.data(),
                                     triangle.size() * sizeof(float), 28);
        auto quadVb =
            MakeBuffer(device, RHIBufferUsage::Vertex, RHIBufferAccess::Dynamic, kFullQuadUv, sizeof(kFullQuadUv), 20);
        auto sampler = MakeSampler(device, RHIFilterMode::Nearest);
        auto offscreen = MakeTarget(device);
        auto target = MakeTarget(device);
        const float gray[4] = {0.2f, 0.2f, 0.2f, 1.0f};
        return RenderFrames(backend, {target.get()},
                            [&](IRHICommandList& cmd)
                            {
                                // Pass 1 renders into a texture that pass 2 then samples: on D3D12 that is a
                                // render-target -> shader-resource transition, and back again next frame.
                                BeginPass(cmd, {offscreen.get()}, nullptr, gray);
                                DrawWith(cmd, colorPipeline, triangleVb.get(), 3);
                                BeginPass(cmd, {target.get()}, nullptr, kBlack);
                                cmd.SetShaderResource(RHIShaderStage::Pixel, 0, offscreen.get());
                                cmd.SetSampler(RHIShaderStage::Pixel, 0, sampler.get());
                                DrawWith(cmd, postPipeline, quadVb.get(), 6);
                            });
    }
    Expectation ExpectRenderToTextureThenSample(uint32_t x, uint32_t y)
    {
        const Expectation source = ColorTriangleAt(x, y, Rgba{51, 51, 51, 255});
        if (!source)
            return std::nullopt;
        return Rgba{static_cast<uint8_t>(255 - source->r), static_cast<uint8_t>(255 - source->g),
                    static_cast<uint8_t>(255 - source->b), 255};
    }

    std::string ReadShippedShader(const char* relativePath)
    {
        std::ifstream file(std::string(SPARK_TEST_SOURCE_DIR) + "/" + relativePath, std::ios::binary);
        ASSERT_TRUE(file.good());
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    std::vector<uint8_t> RenderShippedBloomExtract(ParityBackend& backend)
    {
        IRHIDevice& device = backend.Device();
        const Pipeline pipeline = MakePipeline(device, kUvVS, ReadShippedShader("Shaders/HLSL/BloomExtract.hlsl"),
                                               UvLayout(), {}, "PS_BloomExtract");
        std::vector<float> hdr;
        for (uint32_t y = 0; y < kSize; ++y)
            for (uint32_t x = 0; x < kSize; ++x)
            {
                const auto texel = BloomInput(x, y);
                hdr.insert(hdr.end(), texel.begin(), texel.end());
            }
        auto input =
            MakeTexture(device, kSize, kSize, PixelFormat::R32G32B32A32_FLOAT, RHITextureUsage::ShaderResource);
        device.UpdateTexture(input.get(), hdr.data());
        auto vb =
            MakeBuffer(device, RHIBufferUsage::Vertex, RHIBufferAccess::Dynamic, kFullQuadUv, sizeof(kFullQuadUv), 20);
        auto threshold = MakeConstants(device, {kBloomThreshold, 0.0f, 0.0f, 0.0f});
        auto sampler = MakeSampler(device, RHIFilterMode::Nearest);
        auto target = MakeTarget(device);
        return RenderFrames(backend, {target.get()},
                            [&](IRHICommandList& cmd)
                            {
                                BeginPass(cmd, {target.get()}, nullptr, kBlack);
                                // The shipped shader reads its threshold from b1 (root CBV 1 on D3D12).
                                cmd.SetConstantBuffer(RHIShaderStage::Pixel, 1, threshold.get());
                                cmd.SetShaderResource(RHIShaderStage::Pixel, 0, input.get());
                                cmd.SetSampler(RHIShaderStage::Pixel, 0, sampler.get());
                                DrawWith(cmd, pipeline, vb.get(), 6);
                            });
    }
    Expectation ExpectShippedBloomExtract(uint32_t x, uint32_t y)
    {
        const auto c = BloomInput(x, y);
        const float luminance = c[0] * 0.299f + c[1] * 0.587f + c[2] * 0.114f;
        if (std::abs(luminance - kBloomThreshold) < 0.002f)
            return std::nullopt;
        if (luminance <= kBloomThreshold)
            return Rgba{0, 0, 0, 255};
        return Rgba{Unorm(c[0]), Unorm(c[1]), Unorm(c[2]), 255};
    }

    struct ParityScene
    {
        std::string_view name;
        uint32_t targetCount;
        int analyticTolerance; ///< Per-channel slack against the CPU expectation.
        std::vector<uint8_t> (*render)(ParityBackend&);
        Expectation (*expected)(uint32_t x, uint32_t y); ///< y spans targetCount stacked targets.
    };

    // The declared matrix. Mirrored in wiki/graphics/D3D12-Backend.md (parity-matrix markers).
    constexpr std::array<ParityScene, 12> kScenes = {{
        {"SolidTriangle", 1, 0, RenderSolidTriangle, ExpectSolidTriangle},
        {"VertexColorInterpolation", 1, 2, RenderVertexColorInterpolation, ExpectVertexColorInterpolation},
        {"ConstantBufferColor", 1, 0, RenderConstantBufferColor, ExpectConstantBufferColor},
        {"TexturedQuadPoint", 1, 0, RenderTexturedQuadPoint, ExpectTexturedQuadPoint},
        {"TexturedQuadLinear", 1, 3, RenderTexturedQuadLinear, ExpectTexturedQuadLinear},
        {"DepthTestOrdering", 1, 0, RenderDepthTestOrdering, ExpectDepthTestOrdering},
        {"AlphaBlendOver", 1, 2, RenderAlphaBlendOver, ExpectAlphaBlendOver},
        {"IndexedInstanced", 1, 0, RenderIndexedInstanced, ExpectIndexedInstanced},
        {"ViewportScissor", 1, 0, RenderViewportScissor, ExpectViewportScissor},
        {"MRTClearAndDraw", 2, 0, RenderMRTClearAndDraw, ExpectMRTClearAndDraw},
        {"RenderToTextureThenSample", 1, 2, RenderRenderToTextureThenSample, ExpectRenderToTextureThenSample},
        {"ShippedBloomExtract", 1, 1, RenderShippedBloomExtract, ExpectShippedBloomExtract},
    }};

    // ========================================================================
    // Checks
    // ========================================================================

    int ChannelDistance(const uint8_t* a, Rgba b)
    {
        return std::max({std::abs(a[0] - b.r), std::abs(a[1] - b.g), std::abs(a[2] - b.b), std::abs(a[3] - b.a)});
    }

    /// Every pinned pixel of @p frame matches the scene's CPU expectation, and most pixels are pinned.
    void ExpectAnalytic(const ParityScene& scene, const char* backend, const std::vector<uint8_t>& frame)
    {
        const uint32_t rows = kSize * scene.targetCount;
        ASSERT_TRUE(frame.size() == size_t(kRowPitch) * rows);
        size_t pinned = 0;
        size_t wrong = 0;
        for (uint32_t y = 0; y < rows; ++y)
            for (uint32_t x = 0; x < kSize; ++x)
            {
                const Expectation expected = scene.expected(x, y);
                if (!expected)
                    continue;
                ++pinned;
                const uint8_t* actual = frame.data() + size_t(y) * kRowPitch + x * 4;
                if (ChannelDistance(actual, *expected) > scene.analyticTolerance && wrong++ == 0)
                {
                    std::fprintf(stderr,
                                 "  %s %.*s: first analytic mismatch at (%u, %u): got (%u, %u, %u, %u), "
                                 "expected (%u, %u, %u, %u)\n",
                                 backend, static_cast<int>(scene.name.size()), scene.name.data(), x, y, actual[0],
                                 actual[1], actual[2], actual[3], expected->r, expected->g, expected->b, expected->a);
                }
            }
        EXPECT_EQ(wrong, size_t(0));
        EXPECT_TRUE(pinned * 10 >= size_t(kSize) * rows * 9); // don't-care stays under 10%

        for (uint32_t target = 0; target < scene.targetCount; ++target)
        {
            const auto begin = frame.begin() + static_cast<ptrdiff_t>(target) * kRowPitch * kSize;
            const std::vector<uint8_t> single(begin, begin + static_cast<ptrdiff_t>(kRowPitch) * kSize);
            EXPECT_TRUE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(single, 0.95));
        }
    }

    /// The two backends' frames agree: at most kMaxDifferingFraction of the pixels differ by
    /// more than kChannelTolerance in any channel.
    void ExpectBackendsAgree(const ParityScene& scene, const std::vector<uint8_t>& d3d11,
                             const std::vector<uint8_t>& d3d12)
    {
        ASSERT_TRUE(d3d11.size() == d3d12.size());
        const size_t pixels = d3d11.size() / 4;
        size_t differing = 0;
        int maxDistance = 0;
        for (size_t i = 0; i < pixels; ++i)
        {
            int distance = 0;
            for (size_t c = 0; c < 4; ++c)
                distance = std::max(distance, std::abs(d3d11[i * 4 + c] - d3d12[i * 4 + c]));
            maxDistance = std::max(maxDistance, distance);
            if (distance > kChannelTolerance)
                ++differing;
        }
        std::printf("  %.*s: max channel distance %d, %zu of %zu pixels beyond %d\n",
                    static_cast<int>(scene.name.size()), scene.name.data(), maxDistance, differing, pixels,
                    kChannelTolerance);
        EXPECT_TRUE(static_cast<double>(differing) <= kMaxDifferingFraction * static_cast<double>(pixels));
    }

    /// The wiki's parity-matrix table lists exactly kScenes' names, in order.
    void ExpectWikiMatrixInStep()
    {
        std::ifstream file(std::string(SPARK_TEST_SOURCE_DIR) + "/wiki/graphics/D3D12-Backend.md");
        ASSERT_TRUE(file.good());
        std::vector<std::string> names;
        bool inMatrix = false;
        for (std::string line; std::getline(file, line);)
        {
            if (line.find("<!-- parity-matrix:begin -->") != std::string::npos)
                inMatrix = true;
            else if (line.find("<!-- parity-matrix:end -->") != std::string::npos)
                inMatrix = false;
            else if (inMatrix && line.rfind("| `", 0) == 0)
                names.push_back(line.substr(3, line.find('`', 3) - 3));
        }
        ASSERT_TRUE(names.size() == kScenes.size());
        for (size_t i = 0; i < kScenes.size(); ++i)
            EXPECT_EQ(names[i], std::string(kScenes[i].name));
    }

    void RunScene(std::string_view name)
    {
        ExpectWikiMatrixInStep();
        const auto scene = std::find_if(kScenes.begin(), kScenes.end(), [&](const auto& s) { return s.name == name; });
        ASSERT_TRUE(scene != kScenes.end());

        D3D11Backend d3d11;
        D3D12Backend d3d12;
        ASSERT_TRUE(d3d11.Open());
        ASSERT_TRUE(d3d12.Open());
        const AdapterIdentity a11 = d3d11.Adapter();
        const AdapterIdentity a12 = d3d12.Adapter();
        std::printf("  D3D11 adapter: %s%s | D3D12 adapter: %s%s\n", a11.description.c_str(),
                    a11.software ? " (software)" : "", a12.description.c_str(), a12.software ? " (software)" : "");
        const bool sameAdapter = (a11.luid.LowPart == a12.luid.LowPart && a11.luid.HighPart == a12.luid.HighPart) ||
                                 (a11.software && a12.software);
        if (!sameAdapter)
            std::fprintf(stderr, "  D3D11 and D3D12 run on different adapters (hybrid GPU host?); a cross-GPU "
                                 "comparison could hide or invent a difference, so the scene fails\n");
        ASSERT_TRUE(sameAdapter);

        const std::vector<uint8_t> frame11 = scene->render(d3d11);
        const std::vector<uint8_t> frame12 = scene->render(d3d12);
        ExpectAnalytic(*scene, "D3D11", frame11);
        ExpectAnalytic(*scene, "D3D12", frame12);
        ExpectBackendsAgree(*scene, frame11, frame12);
    }
} // namespace

TEST(D3D12_Parity_SolidTriangle)
{
    RunScene("SolidTriangle");
}

TEST(D3D12_Parity_VertexColorInterpolation)
{
    RunScene("VertexColorInterpolation");
}

TEST(D3D12_Parity_ConstantBufferColor)
{
    RunScene("ConstantBufferColor");
}

TEST(D3D12_Parity_TexturedQuadPoint)
{
    RunScene("TexturedQuadPoint");
}

TEST(D3D12_Parity_TexturedQuadLinear)
{
    RunScene("TexturedQuadLinear");
}

TEST(D3D12_Parity_DepthTestOrdering)
{
    RunScene("DepthTestOrdering");
}

TEST(D3D12_Parity_AlphaBlendOver)
{
    RunScene("AlphaBlendOver");
}

TEST(D3D12_Parity_IndexedInstanced)
{
    RunScene("IndexedInstanced");
}

TEST(D3D12_Parity_ViewportScissor)
{
    RunScene("ViewportScissor");
}

TEST(D3D12_Parity_MRTClearAndDraw)
{
    RunScene("MRTClearAndDraw");
}

TEST(D3D12_Parity_RenderToTextureThenSample)
{
    RunScene("RenderToTextureThenSample");
}

TEST(D3D12_Parity_ShippedBloomExtract)
{
    RunScene("ShippedBloomExtract");
}

#endif // _WIN32 && !SPARK_NO_D3D12
