/**
 * @file TestRHI210D3D11GoldenReal.cpp
 * @brief RHI-210: production-linked D3D11 render-target and golden-frame tests.
 *
 * Playtest issues #577/#579/#582 all ended in a frame that was one flat colour
 * while every "it rendered" signal (draw counters, a written PNG, exit code 0)
 * looked healthy. These tests pin the D3D11 RHI to the stronger claim: pixels
 * that were drawn actually land in the bound target, and a uniform frame is
 * rejected by the golden check instead of passing it.
 *
 *   - D3D11_Resource_WrapNativeRenderTargetCreatesRTV: a wrapped native texture
 *     requested with RenderTarget usage must carry an RTV. Without one,
 *     SetRenderTargets silently binds nullptr and every draw/clear is dropped.
 *   - D3D11_Resource_SetRenderTargetsClampsToEightSlots: a count above the
 *     D3D11 limit (8) read past the local RTV array and made the runtime drop
 *     the whole bind, leaving the previous target bound.
 *   - D3D11_Golden_RHITriangleFrameIsNotUniform: a real RHI draw (shader
 *     compile, pipeline, vertex buffer, Draw) into a WARP/hardware target is
 *     read back and must be non-uniform, with the triangle and clear colours
 *     where geometry says they are.
 *   - D3D11_Golden_UniformFrameIsRejected: the frame check itself fails a
 *     uniform gray frame (the #579 capture colour), so a blank render cannot
 *     pass the golden test above.
 */

#include "TestFramework.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <vector>

#ifdef _WIN32
#include "Graphics/RHI/D3D11/D3D11Device.h"
#include "Graphics/RHI/RHIPipelineTypes.h"
#include "Graphics/RHI/RHITypes.h"
#include <windows.h>
#include <wrl/client.h>
#endif

namespace
{
    /// Minimal golden-frame content check for tightly packed RGBA8 pixels.
    struct FrameContent
    {
        size_t distinctColors = 0;
        double dominantFraction = 1.0; ///< Share of pixels equal to the most common colour
    };

    FrameContent AnalyzeFrame(const std::vector<uint8_t>& rgba)
    {
        FrameContent result;
        const size_t pixelCount = rgba.size() / 4;
        if (pixelCount == 0)
            return result;

        std::vector<uint32_t> packed(pixelCount);
        for (size_t i = 0; i < pixelCount; ++i)
            packed[i] = (uint32_t(rgba[i * 4 + 0]) << 16) | (uint32_t(rgba[i * 4 + 1]) << 8) | rgba[i * 4 + 2];

        std::set<uint32_t> distinct(packed.begin(), packed.end());
        result.distinctColors = distinct.size();

        size_t dominant = 0;
        for (uint32_t color : distinct)
            dominant = std::max<size_t>(dominant, std::count(packed.begin(), packed.end(), color));
        result.dominantFraction = double(dominant) / double(pixelCount);
        return result;
    }

    /// A frame passes only if it has at least two colours and no single colour
    /// covers more than maxDominantFraction of it. A uniform frame always fails.
    bool FrameHasRenderedContent(const std::vector<uint8_t>& rgba, double maxDominantFraction)
    {
        const FrameContent content = AnalyzeFrame(rgba);
        return content.distinctColors >= 2 && content.dominantFraction <= maxDominantFraction;
    }
} // namespace

TEST(D3D11_Golden_UniformFrameIsRejected)
{
    // 64x64 of the uniform (80,76,70) the #579 survival capture produced.
    std::vector<uint8_t> uniform(64 * 64 * 4);
    for (size_t i = 0; i < uniform.size(); i += 4)
    {
        uniform[i + 0] = 80;
        uniform[i + 1] = 76;
        uniform[i + 2] = 70;
        uniform[i + 3] = 255;
    }
    EXPECT_FALSE(FrameHasRenderedContent(uniform, 0.95));

    // One stray pixel does not rescue a blank frame either.
    uniform[0] = 255;
    EXPECT_FALSE(FrameHasRenderedContent(uniform, 0.95));

    // A frame with a real second region passes.
    for (size_t i = 0; i < uniform.size() / 4; i += 4)
        uniform[i] = 200;
    EXPECT_TRUE(FrameHasRenderedContent(uniform, 0.95));
}

#ifdef _WIN32

namespace
{
    using Microsoft::WRL::ComPtr;

    bool TryCreateD3D11Device(Spark::RHI::D3D11::D3D11Device& device)
    {
        Spark::RHI::RHIDeviceDesc desc;
        desc.preferredBackend = Spark::RHI::GraphicsBackend::D3D11;
        desc.applicationName = "SparkTests_RHI210";
        return device.Initialize(desc);
    }

    /// Copies a 2D RGBA8 resource to a staging texture and returns tightly packed pixels.
    std::vector<uint8_t> ReadbackRGBA8(Spark::RHI::D3D11::D3D11Device& device, ID3D11Resource* resource, uint32_t width,
                                       uint32_t height)
    {
        std::vector<uint8_t> pixels;
        D3D11_TEXTURE2D_DESC stagingDesc{};
        stagingDesc.Width = width;
        stagingDesc.Height = height;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device.GetD3D11Device()->CreateTexture2D(&stagingDesc, nullptr, &staging)))
            return pixels;

        ID3D11DeviceContext1* context = device.GetD3D11Context();
        context->CopyResource(staging.Get(), resource);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return pixels;

        pixels.resize(size_t(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y)
        {
            const auto* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
            std::copy(row, row + size_t(width) * 4, pixels.begin() + size_t(y) * width * 4);
        }
        context->Unmap(staging.Get(), 0);
        return pixels;
    }

    const uint8_t* PixelAt(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t x, uint32_t y)
    {
        return rgba.data() + (size_t(y) * width + x) * 4;
    }

    bool PixelNear(const uint8_t* px, int r, int g, int b, int tolerance = 3)
    {
        return std::abs(px[0] - r) <= tolerance && std::abs(px[1] - g) <= tolerance && std::abs(px[2] - b) <= tolerance;
    }

    ComPtr<ID3D11Texture2D> CreateNativeRenderTarget(Spark::RHI::D3D11::D3D11Device& device, uint32_t size)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = size;
        desc.Height = size;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;
        device.GetD3D11Device()->CreateTexture2D(&desc, nullptr, &texture);
        return texture;
    }

    Spark::RHI::RHITextureDesc RenderTargetDesc(uint32_t size, const char* name)
    {
        Spark::RHI::RHITextureDesc desc;
        desc.width = size;
        desc.height = size;
        desc.format = Spark::RHI::PixelFormat::R8G8B8A8_UNORM;
        desc.usage = Spark::RHI::RHITextureUsage::RenderTarget | Spark::RHI::RHITextureUsage::ShaderResource;
        desc.debugName = name;
        return desc;
    }
} // namespace

TEST(D3D11_Resource_WrapNativeRenderTargetCreatesRTV)
{
    Spark::RHI::D3D11::D3D11Device device;
    if (!TryCreateD3D11Device(device))
        SKIP_TEST("No D3D11 device available (hardware or WARP)");

    constexpr uint32_t kSize = 16;
    ComPtr<ID3D11Texture2D> native = CreateNativeRenderTarget(device, kSize);
    ASSERT_TRUE(native != nullptr);

    auto wrapped = device.WrapNativeTexture(native.Get(), RenderTargetDesc(kSize, "RHI210_WrappedRT"));
    ASSERT_TRUE(wrapped != nullptr);

    // At HEAD the wrapper was built with a null RTV whatever the usage said.
    EXPECT_TRUE(wrapped->GetRenderTargetView() != nullptr);

    // Prove the RTV is load-bearing: a clear through the RHI must reach the pixels.
    auto* cmd = device.GetImmediateCommandList();
    ASSERT_TRUE(cmd != nullptr);
    Spark::RHI::IRHITexture* targets[] = {wrapped.get()};
    cmd->SetRenderTargets(targets, 1, nullptr);
    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    cmd->ClearRenderTarget(wrapped.get(), red);

    const auto pixels = ReadbackRGBA8(device, native.Get(), kSize, kSize);
    ASSERT_EQ(pixels.size(), size_t(kSize) * kSize * 4);
    EXPECT_TRUE(PixelNear(PixelAt(pixels, kSize, kSize / 2, kSize / 2), 255, 0, 0));

    wrapped.reset();
    device.Shutdown();
}

TEST(D3D11_Resource_SetRenderTargetsClampsToEightSlots)
{
    Spark::RHI::D3D11::D3D11Device device;
    if (!TryCreateD3D11Device(device))
        SKIP_TEST("No D3D11 device available (hardware or WARP)");

    constexpr uint32_t kSize = 8;
    auto first = device.CreateTexture(RenderTargetDesc(kSize, "RHI210_First"));
    auto second = device.CreateTexture(RenderTargetDesc(kSize, "RHI210_Second"));
    ASSERT_TRUE(first != nullptr && second != nullptr);

    auto* cmd = device.GetImmediateCommandList();
    Spark::RHI::IRHITexture* firstOnly[] = {first.get()};
    cmd->SetRenderTargets(firstOnly, 1, nullptr);

    // Nine entries: more than D3D11 can bind. The caller's intent for slot 0 is
    // `second`; the backend must bind it rather than let the runtime reject the
    // call and leave `first` bound.
    Spark::RHI::IRHITexture* nine[9] = {second.get()};
    cmd->SetRenderTargets(nine, 9, nullptr);

    ID3D11RenderTargetView* bound[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    device.GetD3D11Context()->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, bound, nullptr);
    const bool secondBound = bound[0] == second->GetRenderTargetView();
    for (auto* view : bound)
        if (view)
            view->Release();
    EXPECT_TRUE(secondBound);

    first.reset();
    second.reset();
    device.Shutdown();
}

TEST(D3D11_Golden_RHITriangleFrameIsNotUniform)
{
    Spark::RHI::D3D11::D3D11Device device;
    if (!TryCreateD3D11Device(device))
        SKIP_TEST("No D3D11 device available (hardware or WARP)");

    using namespace Spark::RHI;
    constexpr uint32_t kSize = 64;

    auto target = device.CreateTexture(RenderTargetDesc(kSize, "RHI210_GoldenTarget"));
    ASSERT_TRUE(target != nullptr);
    ASSERT_TRUE(target->GetRenderTargetView() != nullptr);

    RHIShaderDesc vsDesc;
    vsDesc.stage = RHIShaderStage::Vertex;
    vsDesc.debugName = "RHI210_GoldenVS";
    vsDesc.sourceCode = "float4 main(float2 pos : POSITION) : SV_Position { return float4(pos, 0.5f, 1.0f); }\n";
    RHIShaderDesc psDesc;
    psDesc.stage = RHIShaderStage::Pixel;
    psDesc.debugName = "RHI210_GoldenPS";
    psDesc.sourceCode = "float4 main() : SV_Target { return float4(0.9f, 0.2f, 0.1f, 1.0f); }\n";
    auto vs = device.CreateShader(vsDesc);
    auto ps = device.CreateShader(psDesc);
    ASSERT_TRUE(vs != nullptr && ps != nullptr);

    RHIPipelineStateDesc pipelineDesc;
    RHIInputElement position;
    position.semanticName = "POSITION";
    position.format = RHIVertexFormat::Float2;
    pipelineDesc.inputLayout.elements.push_back(position);
    pipelineDesc.rasterizer.cullMode = RHICullMode::None;
    pipelineDesc.depthStencil.depthEnable = false;
    pipelineDesc.depthStencil.depthWrite = false;
    pipelineDesc.renderTargetFormats[0] = PixelFormat::R8G8B8A8_UNORM;
    pipelineDesc.debugName = "RHI210_GoldenPSO";
    auto pipeline = device.CreatePipelineState(pipelineDesc, vs.get(), ps.get());
    ASSERT_TRUE(pipeline != nullptr);

    // Triangle with a 1x1 NDC base/height: 12.5% of the frame, centred.
    const float vertices[] = {-0.5f, -0.5f, 0.0f, 0.5f, 0.5f, -0.5f};
    RHIBufferDesc vbDesc;
    vbDesc.size = sizeof(vertices);
    vbDesc.stride = sizeof(float) * 2;
    vbDesc.usage = RHIBufferUsage::Vertex;
    vbDesc.access = RHIBufferAccess::Static;
    vbDesc.initialData = vertices;
    vbDesc.debugName = "RHI210_GoldenVB";
    auto vb = device.CreateBuffer(vbDesc);
    ASSERT_TRUE(vb != nullptr);

    auto* cmd = device.GetImmediateCommandList();
    IRHITexture* targets[] = {target.get()};
    cmd->SetRenderTargets(targets, 1, nullptr);
    const float clearGray[4] = {0.3f, 0.3f, 0.3f, 1.0f};
    cmd->ClearRenderTarget(target.get(), clearGray);
    RHIViewport viewport;
    viewport.width = float(kSize);
    viewport.height = float(kSize);
    cmd->SetViewport(viewport);
    cmd->SetPipelineState(pipeline.get());
    cmd->SetPrimitiveTopology(RHIPrimitiveTopology::TriangleList);
    cmd->SetVertexBuffer(vb.get(), 0, 0);
    cmd->Draw(3, 0);

    auto* d3dTarget = static_cast<Spark::RHI::D3D11::D3D11Texture*>(target.get());
    const auto pixels = ReadbackRGBA8(device, d3dTarget->GetD3D11Resource(), kSize, kSize);
    ASSERT_EQ(pixels.size(), size_t(kSize) * kSize * 4);

    // The golden assertion: a blank/uniform frame fails here.
    const FrameContent content = AnalyzeFrame(pixels);
    EXPECT_TRUE(FrameHasRenderedContent(pixels, 0.95));

    // Geometry lands where the draw put it: triangle colour in the centre,
    // clear colour in the corners (0.3 -> 77 in UNORM8).
    EXPECT_TRUE(PixelNear(PixelAt(pixels, kSize, kSize / 2, kSize / 2), 230, 51, 26));
    EXPECT_TRUE(PixelNear(PixelAt(pixels, kSize, 1, 1), 77, 77, 77));
    EXPECT_TRUE(PixelNear(PixelAt(pixels, kSize, kSize - 2, kSize - 2), 77, 77, 77));

    // Coverage within a reviewed band around the analytic 12.5%.
    const double triangleFraction = 1.0 - content.dominantFraction;
    EXPECT_GT(triangleFraction, 0.09);
    EXPECT_LT(triangleFraction, 0.16);

    vb.reset();
    pipeline.reset();
    vs.reset();
    ps.reset();
    target.reset();
    device.Shutdown();
}

#endif // _WIN32
