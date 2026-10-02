/**
 * @file TestRHI210D3D11PrimaryGoldenReal.cpp
 * @brief Intermediate deferred geometry/lighting and production shadow-depth goldens.
 *
 * Captures precede the deferred path's forward replay: a working forward renderer
 * cannot mask an absent deferred resolve. The current LightingPass has no resolve;
 * its content assertion must remain red until production writes a lit result.
 * No test shader supplies the missing pass. Pending captures are never baselines.
 */
#include "TestFramework.h"

#ifdef _WIN32
#include "RHI210D3D11EngineFixture.h"
#include "RHI210D3D11GoldenSupport.h"
#include "Game/CubeObject.h"
#include "Graphics/GraphicsRenderPipelinesShadowPass.h"
#include "Utils/GoldenImageManifest.h"
#include <d3d11sdklayers.h>
#include <array>
#include <cstring>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

/// Narrow friend access, defined only in this test translation unit.
struct RHI210GoldenPassAccess
{
    static ID3D11Texture2D* GBuffer(GraphicsEngine& engine, size_t index)
    {
        return engine.m_gBufferTextures[index].Get();
    }

    static void ClearGBuffer(GraphicsEngine& engine)
    {
        const float zero[4] = {};
        for (const auto& target : engine.m_gBufferRTVs)
        {
            ASSERT_TRUE(target != nullptr);
            engine.GetContext()->ClearRenderTargetView(target.Get(), zero);
        }
    }

    static void Geometry(GraphicsEngine& engine, const std::vector<GameObject*>& objects, const XMMATRIX& view,
                         const XMMATRIX& projection)
    {
        engine.FillGBuffer(objects, view, projection);
    }

    static void Lighting(GraphicsEngine& engine, const XMMATRIX& view, const XMMATRIX& projection)
    {
        engine.LightingPass(view, projection);
    }
};

namespace
{
    constexpr const char* kRow = "d3d11-warp";
    const std::array<const char*, 3> kScenes = {"Primary_DeferredGeometry", "Primary_DeferredLighting",
                                                "Primary_ShadowDepth"};
    constexpr uint32_t kWidth = 320;
    constexpr uint32_t kHeight = 240;

    struct TextureBytes
    {
        D3D11_TEXTURE2D_DESC desc{};
        UINT pitch = 0;
        std::vector<uint8_t> bytes;
    };

    TextureBytes ReadTexture(GraphicsEngine& engine, ID3D11Texture2D* texture)
    {
        ASSERT_TRUE(texture != nullptr);
        TextureBytes result;
        texture->GetDesc(&result.desc);
        ASSERT_EQ(result.desc.SampleDesc.Count, 1u);
        auto stagingDesc = result.desc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.MiscFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        ASSERT_TRUE(SUCCEEDED(engine.GetDevice()->CreateTexture2D(&stagingDesc, nullptr, &staging)));
        engine.GetContext()->CopyResource(staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ASSERT_TRUE(SUCCEEDED(engine.GetContext()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
        result.pitch = mapped.RowPitch;
        result.bytes.resize(size_t(result.pitch) * result.desc.Height);
        std::memcpy(result.bytes.data(), mapped.pData, result.bytes.size());
        engine.GetContext()->Unmap(staging.Get(), 0);
        return result;
    }

    RHI210::Frame ColorFrame(const TextureBytes& source)
    {
        ASSERT_TRUE(source.desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
        RHI210::Frame frame{source.desc.Width, source.desc.Height, {}};
        frame.rgba.resize(size_t(frame.width) * frame.height * 4);
        for (uint32_t y = 0; y < frame.height; ++y)
        {
            std::copy_n(source.bytes.data() + size_t(y) * source.pitch, size_t(frame.width) * 4,
                        frame.rgba.data() + size_t(y) * frame.width * 4);
        }
        return frame;
    }

    bool HasWrittenTexels(const TextureBytes& source, size_t bytesPerPixel)
    {
        for (uint32_t y = 0; y < source.desc.Height; ++y)
        {
            const auto* row = source.bytes.data() + size_t(y) * source.pitch;
            if (std::any_of(row, row + source.desc.Width * bytesPerPixel, [](uint8_t value) { return value != 0; }))
            {
                return true;
            }
        }
        return false;
    }

    /// IEEE binary16 material channels, read from the production RGBA16F attachment.
    float HalfChannel(const uint8_t* bytes)
    {
        uint16_t bits = 0;
        std::memcpy(&bits, bytes, sizeof(bits));
        const int exponent = (bits >> 10) & 31;
        const int fraction = bits & 1023;
        ASSERT_TRUE(exponent != 31); // NaN/Inf is never acceptable render output.
        const float value =
            exponent == 0 ? std::ldexp(float(fraction), -24) : std::ldexp(float(1024 + fraction), exponent - 25);
        return (bits & 0x8000) != 0 ? -value : value;
    }

    /// Side-by-side albedo / encoded normal / material RGB. No lighting is simulated.
    RHI210::Frame GeometryFrame(const TextureBytes& albedo, const TextureBytes& normal, const TextureBytes& material)
    {
        ASSERT_TRUE(normal.desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
        ASSERT_TRUE(material.desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
        const auto color = ColorFrame(albedo);
        ASSERT_EQ(normal.desc.Width, color.width);
        ASSERT_EQ(material.desc.Width, color.width);
        ASSERT_EQ(normal.desc.Height, color.height);
        ASSERT_EQ(material.desc.Height, color.height);
        RHI210::Frame result{color.width * 3, color.height,
                             std::vector<uint8_t>(size_t(color.width) * 3 * color.height * 4, 255)};
        for (uint32_t y = 0; y < color.height; ++y)
        {
            for (uint32_t x = 0; x < color.width; ++x)
            {
                const size_t out = (size_t(y) * result.width + x) * 4;
                std::copy_n(color.At(x, y), 4, result.rgba.data() + out);
                uint32_t encoded = 0;
                std::memcpy(&encoded, normal.bytes.data() + size_t(y) * normal.pitch + size_t(x) * 4, sizeof(encoded));
                const auto* channels = material.bytes.data() + size_t(y) * material.pitch + size_t(x) * 8;
                for (size_t channel = 0; channel < 3; ++channel)
                {
                    const float component = float((encoded >> (channel * 10)) & 1023) / 1023.0f;
                    result.rgba[out + color.width * 4 + channel] =
                        static_cast<uint8_t>(RHI210Golden::ToUnorm8(component));
                    result.rgba[out + color.width * 8 + channel] =
                        static_cast<uint8_t>(RHI210Golden::ToUnorm8(HalfChannel(channels + channel * 2)));
                }
            }
        }
        return result;
    }

    void CheckValidation(GraphicsEngine& engine)
    {
        ComPtr<ID3D11InfoQueue> queue;
        ASSERT_TRUE(SUCCEEDED(engine.GetDevice()->QueryInterface(IID_PPV_ARGS(&queue))));
        EXPECT_EQ(queue->GetNumMessagesDiscardedByMessageCountLimit(), 0u);
        const auto counts = engine.GetValidationCounts();
        ASSERT_TRUE(counts.has_value());
        EXPECT_EQ(counts->corruption, 0u);
        EXPECT_EQ(counts->errors, 0u);
        EXPECT_EQ(counts->warnings, 0u);
    }

    void DeferredScene(bool lighting)
    {
        RHI210::WarpGraphicsEngine warp(kWidth, kHeight);
        ASSERT_TRUE(warp.Ready());
        auto& engine = warp.Engine();
        engine.SetRenderingPipeline(RenderingPipeline::Deferred);
        CubeObject cube(1.0f);
        ASSERT_TRUE(SUCCEEDED(cube.Initialize(engine.GetDevice(), engine.GetContext())));
        const XMFLOAT3 eye{1.4f, 1.2f, -2.2f};
        const auto view = XMMatrixLookAtLH(XMLoadFloat3(&eye), XMVectorZero(), XMVectorSet(0, 1, 0, 0));
        const auto projection =
            XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), float(kWidth) / kHeight, 0.1f, 100.0f);
        engine.BeginFrame();
        engine.UpdateFrameConstants(view, projection, eye);
        RHI210GoldenPassAccess::ClearGBuffer(engine);
        if (!RHI210Golden::PassDisabled("deferred-geometry"))
        {
            RHI210GoldenPassAccess::Geometry(engine, {&cube}, view, projection);
        }
        engine.GetContext()->OMSetRenderTargets(0, nullptr, nullptr);
        const auto albedo = ReadTexture(engine, RHI210GoldenPassAccess::GBuffer(engine, 0));
        const auto normal = ReadTexture(engine, RHI210GoldenPassAccess::GBuffer(engine, 1));
        const auto material = ReadTexture(engine, RHI210GoldenPassAccess::GBuffer(engine, 2));
        // An albedo-only draw is not a complete geometry pass. Ignore row padding.
        EXPECT_TRUE(HasWrittenTexels(normal, 4));
        EXPECT_TRUE(HasWrittenTexels(material, 8));
        RHI210::Frame frame = GeometryFrame(albedo, normal, material);
        if (lighting)
        {
            if (!RHI210Golden::PassDisabled("deferred-lighting"))
            {
                RHI210GoldenPassAccess::Lighting(engine, view, projection);
            }
            frame = RHI210::ReadBackBuffer(engine);
        }
        engine.EndFrame();
        ASSERT_EQ(frame.width, lighting ? kWidth : kWidth * 3);
        ASSERT_EQ(frame.height, kHeight);
        EXPECT_TRUE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(frame.rgba, 0.95));
        CheckValidation(engine);
        EXPECT_TRUE(RHI210Golden::MatchesGolden(kRow,
                                                lighting ? "Primary_DeferredLighting" : "Primary_DeferredGeometry",
                                                frame.rgba, frame.width, frame.height));
    }
} // namespace

TEST(D3D11PrimaryGolden_ManifestHasEveryScene)
{
    std::vector<Spark::GoldenManifestEntry> entries;
    std::string error;
    ASSERT_TRUE(Spark::GoldenManifest::Load(RHI210Golden::GoldenDir() / "manifest.json", entries, error));
    for (const char* scene : kScenes)
    {
        const auto entry = std::find_if(entries.begin(), entries.end(), [&](const auto& candidate)
                                        { return candidate.backendRow == kRow && candidate.scene == scene; });
        ASSERT_TRUE(entry != entries.end());
        EXPECT_TRUE(std::filesystem::is_regular_file(RHI210Golden::GoldenDir() / kRow / (std::string(scene) + ".png")));
    }
}

TEST(D3D11PrimaryGolden_DeferredGeometry)
{
    DeferredScene(false);
}

TEST(D3D11PrimaryGolden_DeferredLighting)
{
    DeferredScene(true);
}

TEST(D3D11PrimaryGolden_ShadowDepth)
{
    RHI210::WarpGraphicsEngine warp(kWidth, kHeight);
    ASSERT_TRUE(warp.Ready());
    auto& engine = warp.Engine();
    ASSERT_TRUE(RHI210::LoadCube(engine));
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11DepthStencilView> dsv;
    ASSERT_TRUE(SUCCEEDED(engine.GetDevice()->CreateTexture2D(&desc, nullptr, &depth)));
    ASSERT_TRUE(SUCCEEDED(engine.GetDevice()->CreateDepthStencilView(depth.Get(), nullptr, &dsv)));
    engine.BeginFrame();
    engine.ApplyBasicRenderStates();
    engine.GetContext()->OMSetRenderTargets(0, nullptr, dsv.Get());
    engine.GetContext()->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    engine.SubmitMeshForRendering(RHI210::kCubeMesh, "", XMMatrixRotationY(0.6f), true);
    // The actual depth-only callback used by LightingPass and RenderPipeline::ShadowPass.
    const auto view = XMMatrixLookAtLH(XMVectorSet(2, 3, -4, 1), XMVectorZero(), XMVectorSet(0, 1, 0, 0));
    const auto projection = XMMatrixOrthographicLH(4.0f, 3.0f, 0.1f, 10.0f);
    uint32_t draws = 0;
    if (!RHI210Golden::PassDisabled("shadow"))
    {
        draws = Spark::Graphics::RenderShadowCasterDepth(engine, engine.GetDrawList(), view, projection);
    }
    EXPECT_EQ(draws, 1u);
    engine.GetContext()->OMSetRenderTargets(0, nullptr, nullptr);
    const auto raw = ReadTexture(engine, depth.Get());
    RHI210::Frame frame{kWidth, kHeight, std::vector<uint8_t>(size_t(kWidth) * kHeight * 4, 255)};
    for (uint32_t y = 0; y < kHeight; ++y)
    {
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            float value = 0;
            std::memcpy(&value, raw.bytes.data() + size_t(y) * raw.pitch + size_t(x) * sizeof(float), sizeof(float));
            ASSERT_TRUE(std::isfinite(value) && value >= 0.0f && value <= 1.0f);
            const auto grey = static_cast<uint8_t>(RHI210Golden::ToUnorm8(value));
            const size_t at = (size_t(y) * kWidth + x) * 4;
            frame.rgba[at] = frame.rgba[at + 1] = frame.rgba[at + 2] = grey;
        }
    }
    engine.EndFrame();
    EXPECT_TRUE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(frame.rgba, 0.95));
    CheckValidation(engine);
    EXPECT_TRUE(RHI210Golden::MatchesGolden(kRow, "Primary_ShadowDepth", frame.rgba, frame.width, frame.height));
}
#endif // _WIN32
