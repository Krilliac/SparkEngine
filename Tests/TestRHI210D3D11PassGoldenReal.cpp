/**
 * @file TestRHI210D3D11PassGoldenReal.cpp
 * @brief RHI-210: production D3D11 post-process passes on the d3d11-warp golden row.
 *
 * Each scene drives the production Spark::Graphics::PostProcessingPipeline the
 * way GraphicsEngine::RenderPostProcessing does (SetInputSRV / SetDepthSRV /
 * SetOutputRTV, Process, Render) on a WARP device, with exactly one pass enabled
 * over a fixed 64x64 HDR input. The RGBA8 output is read back and compared with
 * the committed baseline under Tests/GoldenImages/d3d11-warp/ through
 * GoldenImageTestRunner, using the reviewed thresholds and SHA-256 in
 * Tests/GoldenImages/manifest.json (fail-closed).
 *
 * Scenes (the embedded HLSL in PostProcessingPipelineWindowsShaders*.h):
 *   - PostPass_TonemapACES: Tonemapping, ACES operator, default settings.
 *   - PostPass_Bloom: Bloom with threshold 0.6 (default intensity/scatter/radius).
 *   - PostPass_FXAA: FXAA, default edge thresholds.
 *   - PostPass_GTAO: GTAO, default settings, over a flat grey input with a
 *     depth texture holding a sky band, a ground plane and a raised block.
 *
 * Every scene also checks pixels against a CPU evaluation of the pass formula,
 * so a broken render could not have been accepted as a baseline. The shipped
 * runtime enables no post pass by default (every settings struct defaults to
 * enabled = false; pp_enable turns them on), so these four cover the passes a
 * project is most likely to enable, not a default chain.
 *
 * The device is always created with D3D_DRIVER_TYPE_WARP, so the frames belong
 * to the d3d11-warp row on any Windows host. WARP output can differ between
 * Windows builds; a mismatch on another build is data for owner review.
 *
 * On a mismatch the actual frame is written to SPARK_GOLDEN_OUTPUT_DIR (default
 * <cwd>/Tests/Output) as d3d11-warp_<scene>.png next to the runner's _diff.png.
 */

#include "TestFramework.h"

#ifdef _WIN32

#include "Graphics/PostProcessingPipeline.h"
#include "Utils/GoldenImageManifest.h"
#include "Utils/GoldenImageTest.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using Spark::Graphics::PostProcessingPipeline;
using Spark::Graphics::PostProcessPass;

namespace
{
    constexpr const char* kRow = "d3d11-warp";

    /// Every scene this file certifies on kRow. Other d3d11-warp lanes certify
    /// other scenes; Tests/Tools/test_golden_manifest.py checks the union.
    const std::array<const char*, 4> kScenes = {"PostPass_TonemapACES", "PostPass_Bloom", "PostPass_FXAA",
                                                "PostPass_GTAO"};

    constexpr uint32_t kSize = 64;

    using Color3 = std::array<float, 3>;

    std::filesystem::path GoldenDir()
    {
        return std::filesystem::path(SPARK_TEST_SOURCE_DIR) / "Tests" / "GoldenImages";
    }

    std::filesystem::path OutputDir()
    {
        const char* overrideDir = std::getenv("SPARK_GOLDEN_OUTPUT_DIR");
        if (overrideDir != nullptr && overrideDir[0] != '\0')
            return overrideDir;
        return std::filesystem::current_path() / "Tests" / "Output";
    }

    // ------------------------------------------------------------------------
    // Fixed inputs
    // ------------------------------------------------------------------------

    /// HDR tile colours of the 4x4 input (16x16 texels each).
    const std::array<Color3, 16> kTiles = {{{0.05f, 0.05f, 0.05f},
                                            {0.70f, 0.70f, 0.40f},
                                            {0.20f, 0.45f, 0.80f},
                                            {1.60f, 0.90f, 0.30f},
                                            {0.85f, 0.25f, 0.15f},
                                            {0.50f, 0.50f, 0.50f},
                                            {1.00f, 1.00f, 1.00f},
                                            {0.30f, 0.95f, 0.35f},
                                            {0.10f, 0.20f, 0.40f},
                                            {0.90f, 0.80f, 0.20f},
                                            {2.00f, 1.50f, 1.00f},
                                            {0.60f, 0.10f, 0.70f},
                                            {0.00f, 0.00f, 0.00f},
                                            {0.40f, 0.60f, 0.90f},
                                            {3.00f, 2.50f, 2.00f},
                                            {0.75f, 0.65f, 0.55f}}};

    /// Hard-edged disc over the centre tiles: HDR highlight and diagonal edges.
    constexpr Color3 kDisc = {4.0f, 3.2f, 1.0f};
    constexpr float kDiscRadius = 12.0f;

    /// GTAO scene: flat grey colour, sky above kSkyRows, a ground plane and a raised block.
    constexpr Color3 kGrey = {0.8f, 0.8f, 0.8f};
    constexpr uint32_t kSkyRows = 12;
    constexpr float kGroundDepth = 0.5f;
    constexpr float kBlockDepth = 0.3f;
    constexpr uint32_t kBlockMin = 24;
    constexpr uint32_t kBlockMax = 40;

    /// Texel (x, y) of the tile/disc input; row 0 is the top of the frame.
    Color3 InputTexel(uint32_t x, uint32_t y)
    {
        const float dx = float(x) + 0.5f - 32.0f;
        const float dy = float(y) + 0.5f - 32.0f;
        if (dx * dx + dy * dy <= kDiscRadius * kDiscRadius)
            return kDisc;
        return kTiles[(y / 16) * 4 + (x / 16)];
    }

    float GtaoDepth(uint32_t x, uint32_t y)
    {
        if (y < kSkyRows)
            return 0.0f;
        const bool inBlock = x >= kBlockMin && x < kBlockMax && y >= kBlockMin && y < kBlockMax;
        return inBlock ? kBlockDepth : kGroundDepth;
    }

    /// Pixel coordinate of the centre of tile column/row `tile` (0-3).
    constexpr uint32_t TileCentre(uint32_t tile)
    {
        return tile * 16 + 8;
    }

    // ------------------------------------------------------------------------
    // CPU references of the embedded HLSL
    // ------------------------------------------------------------------------

    float Luminance709(const Color3& c)
    {
        return c[0] * 0.2126f + c[1] * 0.7152f + c[2] * 0.0722f;
    }

    float Luma601(const Color3& c)
    {
        return c[0] * 0.299f + c[1] * 0.587f + c[2] * 0.114f;
    }

    /// tonemapPS, op 0 (ACES), exposure/contrast/saturation 1.
    Color3 AcesReference(const Color3& c)
    {
        Color3 out{};
        for (int i = 0; i < 3; ++i)
        {
            const float x = c[i];
            out[i] = std::clamp((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f);
        }
        return out;
    }

    /// bloomPS where all nine taps read colour c: scene + c * contribution * intensity * scatter.
    Color3 BloomFlatReference(const Color3& c, float threshold, float intensity, float scatter)
    {
        const float lum = Luminance709(c);
        const float contribution = std::max(0.0f, lum - threshold) / std::max(lum, 0.00001f);
        Color3 out{};
        for (int i = 0; i < 3; ++i)
            out[i] = c[i] + c[i] * contribution * intensity * scatter;
        return out;
    }

    /// Bilinear sample of the tile/disc input with clamp addressing, as D3D11 filters it.
    Color3 SampleInput(float u, float v)
    {
        const float x = u * float(kSize) - 0.5f;
        const float y = v * float(kSize) - 0.5f;
        const float x0f = std::floor(x);
        const float y0f = std::floor(y);
        const float fx = x - x0f;
        const float fy = y - y0f;
        auto texel = [](float tx, float ty)
        {
            const auto cx = static_cast<uint32_t>(std::clamp(tx, 0.0f, float(kSize - 1)));
            const auto cy = static_cast<uint32_t>(std::clamp(ty, 0.0f, float(kSize - 1)));
            return InputTexel(cx, cy);
        };
        const Color3 a = texel(x0f, y0f);
        const Color3 b = texel(x0f + 1.0f, y0f);
        const Color3 c = texel(x0f, y0f + 1.0f);
        const Color3 d = texel(x0f + 1.0f, y0f + 1.0f);
        Color3 out{};
        for (int i = 0; i < 3; ++i)
        {
            const float top = a[i] + (b[i] - a[i]) * fx;
            const float bottom = c[i] + (d[i] - c[i]) * fx;
            out[i] = top + (bottom - top) * fy;
        }
        return out;
    }

    Color3 Mix(const Color3& a, float wa, const Color3& b, float wb)
    {
        return {a[0] * wa + b[0] * wb, a[1] * wa + b[1] * wb, a[2] * wa + b[2] * wb};
    }

    /// fxaaPS at pixel (px, py) with the default FXAASettings thresholds.
    Color3 FxaaReference(uint32_t px, uint32_t py, bool& tookEdgePath)
    {
        const float texel = 1.0f / float(kSize);
        const float u = (float(px) + 0.5f) * texel;
        const float v = (float(py) + 0.5f) * texel;
        const Spark::Graphics::FXAASettings settings;

        const Color3 m = SampleInput(u, v);
        const Color3 nw = SampleInput(u - texel, v - texel);
        const Color3 ne = SampleInput(u + texel, v - texel);
        const Color3 sw = SampleInput(u - texel, v + texel);
        const Color3 se = SampleInput(u + texel, v + texel);
        const float lumM = Luma601(m);
        const float lumNW = Luma601(nw);
        const float lumNE = Luma601(ne);
        const float lumSW = Luma601(sw);
        const float lumSE = Luma601(se);
        const float lumMin = std::min(lumM, std::min(std::min(lumNW, lumNE), std::min(lumSW, lumSE)));
        const float lumMax = std::max(lumM, std::max(std::max(lumNW, lumNE), std::max(lumSW, lumSE)));
        tookEdgePath = !(lumMax - lumMin < std::max(settings.edgeThresholdMin, lumMax * settings.edgeThreshold));
        if (!tookEdgePath)
            return m;

        float dirX = -((lumNW + lumNE) - (lumSW + lumSE));
        float dirY = (lumNW + lumSW) - (lumNE + lumSE);
        const float dirReduce = std::max((lumNW + lumNE + lumSW + lumSE) * 0.25f * 0.25f, 1.0f / 128.0f);
        const float rcpDirMin = 1.0f / (std::min(std::abs(dirX), std::abs(dirY)) + dirReduce);
        dirX = std::clamp(dirX * rcpDirMin, -8.0f, 8.0f) * texel;
        dirY = std::clamp(dirY * rcpDirMin, -8.0f, 8.0f) * texel;

        auto at = [&](float t) { return SampleInput(u + dirX * t, v + dirY * t); };
        const Color3 rgbA = Mix(at(1.0f / 3.0f - 0.5f), 0.5f, at(2.0f / 3.0f - 0.5f), 0.5f);
        const Color3 outer = Mix(at(-0.5f), 0.25f, at(0.5f), 0.25f);
        const Color3 rgbB = {rgbA[0] * 0.5f + outer[0], rgbA[1] * 0.5f + outer[1], rgbA[2] * 0.5f + outer[2]};
        const float lumB = Luma601(rgbB);
        return (lumB < lumMin || lumB > lumMax) ? rgbA : rgbB;
    }

    /// gtaoPS on a flat plane whose every horizon sample stays on the plane: each
    /// horizon is at pi/2, so vis = (1 + pi) / (2 pi) per direction, raised to power.
    float GtaoFlatPlaneFactor(float power)
    {
        constexpr float kPi = 3.14159265f;
        return std::pow((1.0f + kPi) / (2.0f * kPi), power);
    }

    // ------------------------------------------------------------------------
    // Pixel checks
    // ------------------------------------------------------------------------

    std::array<int, 3> PixelAt(const std::vector<uint8_t>& rgba, uint32_t x, uint32_t y)
    {
        const size_t i = (size_t(y) * kSize + x) * 4;
        return {rgba[i + 0], rgba[i + 1], rgba[i + 2]};
    }

    int ToUnorm8(float value)
    {
        return static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    }

    /// Pixel (x, y) matches the expected colour (saturated to UNORM8) within `tolerance` per channel.
    bool PixelMatches(const std::vector<uint8_t>& rgba, uint32_t x, uint32_t y, const Color3& expected,
                      int tolerance = 1)
    {
        const auto px = PixelAt(rgba, x, y);
        bool ok = true;
        for (int c = 0; c < 3; ++c)
            ok = ok && std::abs(px[c] - ToUnorm8(expected[c])) <= tolerance;
        if (!ok)
            std::printf("[RHI-210 PASS GOLDEN] pixel (%u,%u) = (%d,%d,%d), expected (%d,%d,%d) +/-%d\n", x, y, px[0],
                        px[1], px[2], ToUnorm8(expected[0]), ToUnorm8(expected[1]), ToUnorm8(expected[2]), tolerance);
        return ok;
    }

    // ------------------------------------------------------------------------
    // Golden comparison
    // ------------------------------------------------------------------------

    /// Hands a frame that was already read back to GoldenImageTestRunner.
    class ReadbackCapture final : public Spark::IGoldenImageCapture
    {
      public:
        explicit ReadbackCapture(std::vector<uint8_t> pixels) : m_pixels(std::move(pixels)) {}

        std::vector<uint8_t> CaptureFramebuffer(uint32_t width, uint32_t height) override
        {
            // A size other than the rendered one fails the runner's size check.
            if (width != kSize || height != kSize)
                return {};
            return m_pixels;
        }

      private:
        std::vector<uint8_t> m_pixels;
    };

    /// Compares a frame with the reviewed baseline; keeps the actual frame for review on failure.
    bool MatchesGolden(const char* scene, const std::vector<uint8_t>& pixels)
    {
        auto& runner = Spark::GoldenImageTestRunner::GetInstance();
        Spark::GoldenImageConfig config;
        config.goldenImageDir = GoldenDir().string();
        config.outputDir = OutputDir().string();
        config.backendRow = kRow;
        runner.Initialize(config);
        runner.SetCapture(std::make_unique<ReadbackCapture>(pixels));
        const Spark::ImageComparisonResult result = runner.CompareWithGolden(scene);
        runner.Shutdown();

        std::printf("[RHI-210 PASS GOLDEN] scene=%s matched=%s differing=%u/%u (%.3f%%, tolerance %.3f%%) "
                    "maxDist=%.2f meanDist=%.3f threshold=%.2f%s%s\n",
                    scene, result.matched ? "yes" : "no", result.differentPixels, result.totalPixels,
                    result.percentDifferent, result.tolerancePercent, result.maxPixelDistance,
                    result.averagePixelDistance, result.perPixelThreshold,
                    result.failureReason.empty() ? "" : " reason=", result.failureReason.c_str());

        if (!result.matched)
        {
            // The runner keeps the actual frame only on a pixel failure; keep it on every
            // failure (missing entry, hash mismatch) so a new baseline can be reviewed.
            const std::filesystem::path actual = OutputDir() / (std::string(kRow) + "_" + scene + ".png");
            std::error_code ec;
            std::filesystem::create_directories(actual.parent_path(), ec);
            if (Spark::GoldenImageTestRunner::SavePNG(actual.string(), pixels.data(), kSize, kSize))
                std::printf("[RHI-210 PASS GOLDEN] actual frame written to %s\n", actual.string().c_str());
        }
        return result.matched;
    }

    // ------------------------------------------------------------------------
    // WARP device and the production pipeline
    // ------------------------------------------------------------------------

    struct WarpScene
    {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<ID3D11ShaderResourceView> colorSRV;
        ComPtr<ID3D11ShaderResourceView> depthSRV;
        ComPtr<ID3D11Texture2D> outputTexture;
        ComPtr<ID3D11RenderTargetView> outputRTV;
    };

    bool CreateTexture(ID3D11Device* device, DXGI_FORMAT format, const void* data, UINT pitch,
                       ComPtr<ID3D11ShaderResourceView>& srv)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = kSize;
        desc.Height = kSize;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA initial{data, pitch, 0};
        ComPtr<ID3D11Texture2D> texture;
        return SUCCEEDED(device->CreateTexture2D(&desc, &initial, &texture)) &&
               SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &srv));
    }

    /// WARP device, RGBA32F colour input (tiles and disc, or flat grey for GTAO),
    /// R32F depth for GTAO, and an RGBA8 output target like the swap-chain buffer.
    bool CreateWarpScene(WarpScene& scene, bool gtaoInput)
    {
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                                     &scene.device, nullptr, &scene.context)))
            return false;

        std::vector<float> color(size_t(kSize) * kSize * 4);
        std::vector<float> depth(size_t(kSize) * kSize);
        for (uint32_t y = 0; y < kSize; ++y)
        {
            for (uint32_t x = 0; x < kSize; ++x)
            {
                const Color3 c = gtaoInput ? kGrey : InputTexel(x, y);
                float* texel = &color[(size_t(y) * kSize + x) * 4];
                texel[0] = c[0];
                texel[1] = c[1];
                texel[2] = c[2];
                texel[3] = 1.0f;
                depth[size_t(y) * kSize + x] = GtaoDepth(x, y);
            }
        }
        if (!CreateTexture(scene.device.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, color.data(), kSize * 16,
                           scene.colorSRV) ||
            !CreateTexture(scene.device.Get(), DXGI_FORMAT_R32_FLOAT, depth.data(), kSize * 4, scene.depthSRV))
            return false;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = kSize;
        desc.Height = kSize;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        return SUCCEEDED(scene.device->CreateTexture2D(&desc, nullptr, &scene.outputTexture)) &&
               SUCCEEDED(scene.device->CreateRenderTargetView(scene.outputTexture.Get(), nullptr, &scene.outputRTV));
    }

    std::vector<uint8_t> ReadOutput(WarpScene& scene)
    {
        D3D11_TEXTURE2D_DESC desc{};
        scene.outputTexture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        std::vector<uint8_t> pixels;
        if (FAILED(scene.device->CreateTexture2D(&desc, nullptr, &staging)))
            return pixels;
        scene.context->CopyResource(staging.Get(), scene.outputTexture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(scene.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return pixels;
        pixels.resize(size_t(kSize) * kSize * 4);
        for (uint32_t y = 0; y < kSize; ++y)
        {
            const auto* row = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
            std::copy_n(row, size_t(kSize) * 4, pixels.begin() + static_cast<std::ptrdiff_t>(y * kSize * 4));
        }
        scene.context->Unmap(staging.Get(), 0);
        return pixels;
    }

    /// Runs one enabled pass through the production pipeline, exactly as
    /// GraphicsEngine::RenderPostProcessing drives it, and reads the output back.
    template <typename Configure>
    std::vector<uint8_t> RunSinglePass(PostProcessPass pass, bool gtaoInput, Configure&& configure)
    {
        WarpScene scene;
        if (!CreateWarpScene(scene, gtaoInput))
            throw std::runtime_error("could not create the WARP device or the scene textures");

        D3D11_VIEWPORT viewport{};
        viewport.Width = float(kSize);
        viewport.Height = float(kSize);
        viewport.MaxDepth = 1.0f;
        scene.context->RSSetViewports(1, &viewport);

        PostProcessingPipeline pipeline;
        pipeline.SetDevice(scene.device.Get(), scene.context.Get());
        if (!pipeline.Initialize(kSize, kSize))
            throw std::runtime_error("PostProcessingPipeline::Initialize failed on WARP");
        pipeline.SetEffectEnabled(pass, true);
        configure(pipeline);

        pipeline.SetInputSRV(scene.colorSRV.Get());
        pipeline.SetDepthSRV(gtaoInput ? scene.depthSRV.Get() : nullptr);
        pipeline.SetOutputRTV(scene.outputRTV.Get());
        pipeline.Process(1.0f / 60.0f);
        const int activePasses = pipeline.GetActivePassCount();
        pipeline.Render();
        std::vector<uint8_t> pixels = ReadOutput(scene);
        pipeline.Shutdown();

        // A pass whose shader failed to compile or bind is skipped silently by
        // Process(); the output would then be a plain copy of the input.
        if (activePasses != 1)
            throw std::runtime_error(
                "the enabled pass did not execute (active passes: " + std::to_string(activePasses) + ")");
        if (pixels.size() != size_t(kSize) * kSize * 4)
            throw std::runtime_error("output readback failed");
        return pixels;
    }
} // namespace

// ----------------------------------------------------------------------------
// Every scene this lane certifies has a reviewed software-row entry and PNG.
// ----------------------------------------------------------------------------
TEST(D3D11PassGolden_ManifestHasEveryScene)
{
    std::vector<Spark::GoldenManifestEntry> entries;
    std::string error;
    ASSERT_TRUE(Spark::GoldenManifest::Load(GoldenDir() / "manifest.json", entries, error));
    for (const char* scene : kScenes)
    {
        const auto entry = std::find_if(entries.begin(), entries.end(), [&](const Spark::GoldenManifestEntry& e)
                                        { return e.backendRow == kRow && e.scene == scene; });
        if (entry == entries.end())
            std::printf("[RHI-210 PASS GOLDEN] scene without a manifest entry: %s\n", scene);
        ASSERT_TRUE(entry != entries.end());
        EXPECT_TRUE(entry->software);
        EXPECT_TRUE(std::filesystem::is_regular_file(GoldenDir() / kRow / (std::string(scene) + ".png")));
    }
}

// ----------------------------------------------------------------------------
// Tonemapping (ACES): HDR tiles and the disc map through the ACES fit.
// ----------------------------------------------------------------------------
TEST(D3D11PassGolden_TonemapACES)
{
    const auto frame = RunSinglePass(PostProcessPass::Tonemapping, false,
                                     [](PostProcessingPipeline& pipeline)
                                     {
                                         auto& settings = pipeline.GetTonemappingSettings();
                                         settings.op = Spark::Graphics::TonemapOperator::ACES;
                                     });
    EXPECT_TRUE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(frame, 0.5));
    EXPECT_TRUE(PixelMatches(frame, TileCentre(1), TileCentre(0), AcesReference(kTiles[1])));
    EXPECT_TRUE(PixelMatches(frame, TileCentre(2), TileCentre(3), AcesReference(kTiles[14])));
    EXPECT_TRUE(PixelMatches(frame, TileCentre(3), TileCentre(0), AcesReference(kTiles[3])));
    EXPECT_TRUE(PixelMatches(frame, 32, 32, AcesReference(kDisc)));
    EXPECT_TRUE(MatchesGolden("PostPass_TonemapACES", frame));
}

// ----------------------------------------------------------------------------
// Bloom: bright-pass contribution above the threshold, unchanged below it.
// ----------------------------------------------------------------------------
TEST(D3D11PassGolden_Bloom)
{
    constexpr float kThreshold = 0.6f;
    const Spark::Graphics::BloomSettings defaults;
    const auto frame = RunSinglePass(PostProcessPass::Bloom, false, [&](PostProcessingPipeline& pipeline)
                                     { pipeline.GetBloomSettings().threshold = kThreshold; });
    EXPECT_TRUE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(frame, 0.5));

    // All nine taps (radius 4 texels) stay inside these tiles.
    auto reference = [&](const Color3& c)
    { return BloomFlatReference(c, kThreshold, defaults.intensity, defaults.scatter); };
    EXPECT_TRUE(PixelMatches(frame, TileCentre(1), TileCentre(0), reference(kTiles[1])));
    EXPECT_TRUE(PixelMatches(frame, TileCentre(3), TileCentre(3), reference(kTiles[15])));
    // Below the threshold: luminance 0.579 passes through unchanged.
    EXPECT_TRUE(PixelMatches(frame, TileCentre(1), TileCentre(3), kTiles[13]));
    // The brightened tile really moved: the pass is not a copy.
    EXPECT_GT(PixelAt(frame, TileCentre(1), TileCentre(0))[0], ToUnorm8(kTiles[1][0]) + 8);
    EXPECT_TRUE(MatchesGolden("PostPass_Bloom", frame));
}

// ----------------------------------------------------------------------------
// FXAA: flat regions take the early-out, the disc edge is filtered.
// ----------------------------------------------------------------------------
TEST(D3D11PassGolden_FXAA)
{
    const auto frame = RunSinglePass(PostProcessPass::FXAA, false, [](PostProcessingPipeline&) {});
    EXPECT_TRUE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(frame, 0.5));

    bool edge = true;
    EXPECT_TRUE(PixelMatches(frame, TileCentre(1), TileCentre(0), FxaaReference(TileCentre(1), TileCentre(0), edge)));
    EXPECT_FALSE(edge);
    EXPECT_TRUE(PixelMatches(frame, TileCentre(1), TileCentre(3), FxaaReference(TileCentre(1), TileCentre(3), edge)));
    EXPECT_FALSE(edge);

    // Disc edge against the grey tile (upper left): the filtered blend, not the input texel.
    constexpr uint32_t kEdgeX = 23;
    constexpr uint32_t kEdgeY = 24;
    const Color3 filtered = FxaaReference(kEdgeX, kEdgeY, edge);
    EXPECT_TRUE(edge);
    EXPECT_TRUE(PixelMatches(frame, kEdgeX, kEdgeY, filtered, 2));
    // The filtered blue channel is well away from the input texel's, so a pass
    // that copied its input could not match the reference above.
    EXPECT_GT(std::abs(ToUnorm8(filtered[2]) - ToUnorm8(InputTexel(kEdgeX, kEdgeY)[2])), 10);
    EXPECT_TRUE(MatchesGolden("PostPass_FXAA", frame));
}

// ----------------------------------------------------------------------------
// GTAO: sky passes through, the flat plane gets the flat-horizon factor, and
// the plane next to the raised block is occluded further.
// ----------------------------------------------------------------------------
TEST(D3D11PassGolden_GTAO)
{
    const Spark::Graphics::GTAOSettings defaults;
    const auto frame = RunSinglePass(PostProcessPass::GTAO, true, [](PostProcessingPipeline&) {});
    EXPECT_TRUE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(frame, 0.8));

    const float flat = GtaoFlatPlaneFactor(defaults.power);
    const Color3 flatColor = {kGrey[0] * flat, kGrey[1] * flat, kGrey[2] * flat};
    EXPECT_TRUE(PixelMatches(frame, 32, 4, kGrey));
    EXPECT_TRUE(PixelMatches(frame, 56, 56, flatColor));
    EXPECT_TRUE(PixelMatches(frame, 8, 56, flatColor));
    // The plane texel beside the raised block sees it as an occluder.
    const int nextToBlock = PixelAt(frame, kBlockMin - 1, 32)[0];
    std::printf("[RHI-210 PASS GOLDEN] GTAO flat plane %d, next to block %d\n", ToUnorm8(flatColor[0]), nextToBlock);
    EXPECT_LT(nextToBlock, ToUnorm8(flatColor[0]) - 20);
    EXPECT_TRUE(MatchesGolden("PostPass_GTAO", frame));
}

#endif // _WIN32
