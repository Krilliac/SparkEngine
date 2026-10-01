/**
 * @file TestRHI210D3D11ValidationReal.cpp
 * @brief RHI-210: the D3D11 debug layer validates real engine and RHI frames.
 *
 * Release builds used to create the D3D11 device without the debug layer, so
 * nothing checked the frames CI renders. SPARK_D3D11_DEBUG_LAYER=1 now turns it
 * on in any build and GraphicsEngine counts its messages by severity:
 *
 *   - D3D11_Validation_EngineFramesAreClean: a WARP GraphicsEngine in a hidden
 *     window renders a lit, shadow-casting cube with post-processing for 16
 *     frames around one resize; the layer must report no corruption and no errors.
 *   - D3D11_Validation_CounterSeesInjectedError: the negative control. An invalid
 *     CreateBuffer (ByteWidth 0) through the engine's device must raise the error
 *     count, so a clean result above is a real observation, not a dead counter.
 *   - D3D11_Validation_RHIGoldenTriangleIsClean: the D3D11_Golden triangle drawn
 *     through a debug-layer D3D11Device is correct and raises no errors.
 *   - D3D11_Validation_CreateDestroyStressReturnsToBaseline: 2,000 texture/buffer/
 *     pipeline create-destroy cycles on a debug-layer D3D11Device raise no errors
 *     and leave the layer's live-object count where it started.
 *
 * The layer needs the Windows "Graphics Tools" optional feature. These tests fail
 * (never skip) without it, so the dedicated D3D11_Validation CTest lane cannot
 * pass unvalidated; the family is excluded from the main SparkEngineTests run.
 */

#include "TestFramework.h"

#ifdef _WIN32

#include "RHI210D3D11EngineFixture.h"
#include "Utils/GoldenImageTest.h"

#include "Graphics/RHI/D3D11/D3D11Device.h"
#include "Graphics/RHI/RHIPipelineTypes.h"
#include "Graphics/RHI/RHITypes.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <vector>

namespace
{
    constexpr const char* kMissingLayer =
        "GraphicsEngine::Initialize failed with SPARK_D3D11_DEBUG_LAYER=1; is the Windows 'Graphics Tools' "
        "optional feature (D3D11 debug layer) installed?";

    /// Every live D3D11 object of @p device, as reported one message per object by
    /// ReportLiveDeviceObjects(DETAIL). Deferred destruction is flushed first.
    uint64_t CountLiveObjects(Spark::RHI::D3D11::D3D11Device& device, ID3D11Debug* debug)
    {
        ID3D11InfoQueue* queue = device.GetInfoQueue();
        device.GetD3D11Context()->ClearState();
        device.GetD3D11Context()->Flush();
        queue->ClearStoredMessages();
        // D3D11_RLDO_IGNORE_INTERNAL; mingw-w64's d3d11sdklayers.h predates that enumerator.
        constexpr UINT kRldoIgnoreInternal = 0x4;
        debug->ReportLiveDeviceObjects(static_cast<D3D11_RLDO_FLAGS>(D3D11_RLDO_DETAIL | kRldoIgnoreInternal));
        const uint64_t live = queue->GetNumStoredMessages();
        queue->ClearStoredMessages();
        return live;
    }
} // namespace

TEST(D3D11_Validation_EngineFramesAreClean)
{
    RHI210::ScopedEnvironmentVariable warp(L"SPARK_D3D11_DRIVER", L"warp");
    RHI210::ScopedEnvironmentVariable debugLayer(L"SPARK_D3D11_DEBUG_LAYER", L"1");
    RHI210::HiddenWindow window;
    ASSERT_TRUE(window.Get() != nullptr);

    GraphicsEngine engine;
    const HRESULT hr = engine.Initialize(window.Get());
    if (FAILED(hr))
        std::cerr << "  " << kMissingLayer << "\n";
    ASSERT_TRUE(SUCCEEDED(hr));

    // The layer must actually be observing, or "zero errors" means nothing.
    ASSERT_TRUE(engine.GetValidationCounts().has_value());
    ASSERT_TRUE(RHI210::LoadCube(engine));

    RHI210::Frame last;
    for (int frame = 0; frame < 8; ++frame)
        last = RHI210::RenderCubeFrame(engine);
    engine.OnResize(400, 300);
    for (int frame = 0; frame < 8; ++frame)
        last = RHI210::RenderCubeFrame(engine);

    // The frames were real: the resized back buffer holds a drawn cube.
    EXPECT_EQ(last.width, 400u);
    EXPECT_EQ(last.height, 300u);
    EXPECT_TRUE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(last.rgba, 0.95));

    const std::optional<GraphicsEngine::ValidationCounts> counts = engine.GetValidationCounts();
    ASSERT_TRUE(counts.has_value());
    EXPECT_EQ(counts->corruption, 0u);
    EXPECT_EQ(counts->errors, 0u);
    engine.Shutdown();
}

TEST(D3D11_Validation_CounterSeesInjectedError)
{
    RHI210::ScopedEnvironmentVariable warp(L"SPARK_D3D11_DRIVER", L"warp");
    RHI210::ScopedEnvironmentVariable debugLayer(L"SPARK_D3D11_DEBUG_LAYER", L"1");
    RHI210::HiddenWindow window;
    ASSERT_TRUE(window.Get() != nullptr);

    GraphicsEngine engine;
    const HRESULT hr = engine.Initialize(window.Get());
    if (FAILED(hr))
        std::cerr << "  " << kMissingLayer << "\n";
    ASSERT_TRUE(SUCCEEDED(hr));

    const std::optional<GraphicsEngine::ValidationCounts> before = engine.GetValidationCounts();
    ASSERT_TRUE(before.has_value());

    // A zero-sized vertex buffer is invalid; the runtime refuses it and the debug
    // layer reports CREATEBUFFER_INVALIDDIMENSIONS at ERROR severity.
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    EXPECT_TRUE(FAILED(engine.GetDevice()->CreateBuffer(&desc, nullptr, &buffer)));

    const std::optional<GraphicsEngine::ValidationCounts> after = engine.GetValidationCounts();
    ASSERT_TRUE(after.has_value());
    EXPECT_GE(after->errors, before->errors + 1);
    engine.Shutdown();
}

TEST(D3D11_Validation_RHIGoldenTriangleIsClean)
{
    using namespace Spark::RHI;

    D3D11::D3D11Device device;
    RHIDeviceDesc deviceDesc;
    deviceDesc.preferredBackend = GraphicsBackend::D3D11;
    deviceDesc.enableDebugLayer = true;
    deviceDesc.applicationName = "SparkTests_RHI210_Validation";
    const bool initialized = device.Initialize(deviceDesc);
    if (!initialized)
        std::cerr << "  D3D11Device::Initialize failed with enableDebugLayer; is the D3D11 debug layer installed?\n";
    ASSERT_TRUE(initialized);
    ID3D11InfoQueue* queue = device.GetInfoQueue();
    ASSERT_TRUE(queue != nullptr);
    queue->ClearStoredMessages();

    constexpr uint32_t kSize = 64;
    RHITextureDesc targetDesc;
    targetDesc.width = kSize;
    targetDesc.height = kSize;
    targetDesc.format = PixelFormat::R8G8B8A8_UNORM;
    targetDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
    targetDesc.debugName = "RHI210_ValidationTarget";
    auto target = device.CreateTexture(targetDesc);
    ASSERT_TRUE(target != nullptr);

    RHIShaderDesc vsDesc;
    vsDesc.stage = RHIShaderStage::Vertex;
    vsDesc.debugName = "RHI210_ValidationVS";
    vsDesc.sourceCode = "float4 main(float2 pos : POSITION) : SV_Position { return float4(pos, 0.5f, 1.0f); }\n";
    RHIShaderDesc psDesc;
    psDesc.stage = RHIShaderStage::Pixel;
    psDesc.debugName = "RHI210_ValidationPS";
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
    pipelineDesc.debugName = "RHI210_ValidationPSO";
    auto pipeline = device.CreatePipelineState(pipelineDesc, vs.get(), ps.get());
    ASSERT_TRUE(pipeline != nullptr);

    const float vertices[] = {-0.5f, -0.5f, 0.0f, 0.5f, 0.5f, -0.5f};
    RHIBufferDesc vbDesc;
    vbDesc.size = sizeof(vertices);
    vbDesc.stride = sizeof(float) * 2;
    vbDesc.usage = RHIBufferUsage::Vertex;
    vbDesc.access = RHIBufferAccess::Static;
    vbDesc.initialData = vertices;
    vbDesc.debugName = "RHI210_ValidationVB";
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

    // Read the centre texel: the draw must have landed, or a clean queue proves nothing.
    D3D11_TEXTURE2D_DESC stagingDesc{};
    stagingDesc.Width = kSize;
    stagingDesc.Height = kSize;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    ASSERT_TRUE(SUCCEEDED(device.GetD3D11Device()->CreateTexture2D(&stagingDesc, nullptr, &staging)));
    auto* d3dTarget = static_cast<D3D11::D3D11Texture*>(target.get());
    ID3D11DeviceContext1* context = device.GetD3D11Context();
    context->CopyResource(staging.Get(), d3dTarget->GetD3D11Resource());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    ASSERT_TRUE(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
    const auto* centre =
        static_cast<const uint8_t*>(mapped.pData) + size_t(kSize / 2) * mapped.RowPitch + size_t(kSize / 2) * 4;
    const bool triangleAtCentre =
        std::abs(centre[0] - 230) <= 3 && std::abs(centre[1] - 51) <= 3 && std::abs(centre[2] - 26) <= 3;
    context->Unmap(staging.Get(), 0);
    EXPECT_TRUE(triangleAtCentre);

    // Nothing may have been dropped unclassified by the queue's storage limit.
    EXPECT_EQ(queue->GetNumMessagesDiscardedByMessageCountLimit(), 0u);
    GraphicsEngine::ValidationCounts counts;
    GraphicsEngine::AccumulateValidationMessages(queue, counts);
    EXPECT_EQ(counts.corruption, 0u);
    EXPECT_EQ(counts.errors, 0u);

    vb.reset();
    pipeline.reset();
    vs.reset();
    ps.reset();
    target.reset();
    device.Shutdown();
}

TEST(D3D11_Validation_CreateDestroyStressReturnsToBaseline)
{
    using namespace Spark::RHI;

    D3D11::D3D11Device device;
    RHIDeviceDesc deviceDesc;
    deviceDesc.preferredBackend = GraphicsBackend::D3D11;
    deviceDesc.enableDebugLayer = true;
    deviceDesc.applicationName = "SparkTests_RHI210_Stress";
    const bool initialized = device.Initialize(deviceDesc);
    if (!initialized)
        std::cerr << "  D3D11Device::Initialize failed with enableDebugLayer; is the D3D11 debug layer installed?\n";
    ASSERT_TRUE(initialized);
    ASSERT_TRUE(device.GetInfoQueue() != nullptr);
    Microsoft::WRL::ComPtr<ID3D11Debug> debug;
    ASSERT_TRUE(SUCCEEDED(device.GetD3D11Device()->QueryInterface(IID_PPV_ARGS(&debug))));

    RHIShaderDesc vsDesc;
    vsDesc.stage = RHIShaderStage::Vertex;
    vsDesc.debugName = "RHI210_StressVS";
    vsDesc.sourceCode = "float4 main(float2 pos : POSITION) : SV_Position { return float4(pos, 0.5f, 1.0f); }\n";
    RHIShaderDesc psDesc;
    psDesc.stage = RHIShaderStage::Pixel;
    psDesc.debugName = "RHI210_StressPS";
    psDesc.sourceCode = "float4 main() : SV_Target { return float4(0.2f, 0.6f, 0.9f, 1.0f); }\n";
    auto vs = device.CreateShader(vsDesc);
    auto ps = device.CreateShader(psDesc);
    ASSERT_TRUE(vs != nullptr && ps != nullptr);

    RHIPipelineStateDesc pipelineDesc;
    RHIInputElement position;
    position.semanticName = "POSITION";
    position.format = RHIVertexFormat::Float2;
    pipelineDesc.inputLayout.elements.push_back(position);
    pipelineDesc.renderTargetFormats[0] = PixelFormat::R8G8B8A8_UNORM;
    pipelineDesc.debugName = "RHI210_StressPSO";

    const float vertices[] = {-0.5f, -0.5f, 0.0f, 0.5f, 0.5f, -0.5f};
    RHIBufferDesc bufferDesc;
    bufferDesc.size = sizeof(vertices);
    bufferDesc.stride = sizeof(float) * 2;
    bufferDesc.usage = RHIBufferUsage::Vertex;
    bufferDesc.access = RHIBufferAccess::Static;
    bufferDesc.initialData = vertices;
    bufferDesc.debugName = "RHI210_StressVB";

    RHITextureDesc textureDesc;
    textureDesc.width = 64;
    textureDesc.height = 64;
    textureDesc.format = PixelFormat::R8G8B8A8_UNORM;
    textureDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
    textureDesc.debugName = "RHI210_StressRT";

    const uint64_t baseline = CountLiveObjects(device, debug.Get());
    ASSERT_TRUE(baseline > 0);

    // Control: the count sees one extra live object, so an unchanged count after
    // the loop is a measurement, not a counter that stopped counting.
    {
        auto held = device.CreateBuffer(bufferDesc);
        ASSERT_TRUE(held != nullptr);
        EXPECT_GT(CountLiveObjects(device, debug.Get()), baseline);
    }
    EXPECT_EQ(CountLiveObjects(device, debug.Get()), baseline);

    constexpr int kCycles = 2000;
    int created = 0;
    GraphicsEngine::ValidationCounts counts;
    for (int cycle = 0; cycle < kCycles; ++cycle)
    {
        auto texture = device.CreateTexture(textureDesc);
        auto buffer = device.CreateBuffer(bufferDesc);
        auto pipeline = device.CreatePipelineState(pipelineDesc, vs.get(), ps.get());
        if (texture && buffer && pipeline)
            ++created;
        // Drain every cycle: the queue keeps its default storage limit, and a
        // message dropped by it would never be classified.
        GraphicsEngine::AccumulateValidationMessages(device.GetInfoQueue(), counts);
    }
    EXPECT_EQ(created, kCycles);
    EXPECT_EQ(device.GetInfoQueue()->GetNumMessagesDiscardedByMessageCountLimit(), 0u);
    EXPECT_EQ(counts.corruption, 0u);
    EXPECT_EQ(counts.errors, 0u);

    EXPECT_EQ(CountLiveObjects(device, debug.Get()), baseline);

    vs.reset();
    ps.reset();
    debug.Reset();
    device.Shutdown();
}

#endif // _WIN32
