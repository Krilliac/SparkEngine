/**
 * @file TestRHI230VulkanGoldenReal.cpp
 * @brief RHI-230: shipped-shader Vulkan goldens on the vulkan-lavapipe row.
 *
 * Each test renders a deterministic input through a real VulkanDevice (with
 * VK_LAYER_KHRONOS_validation on) using the SPIR-V the build compiles from
 * Shaders/GLSL (root CMakeLists.txt section 9.4), reads the render target back,
 * and compares it with the committed baseline under
 * Tests/GoldenImages/vulkan-lavapipe/ using the reviewed thresholds and baseline
 * SHA-256 in Tests/GoldenImages/manifest.json (GoldenImageTestRunner,
 * fail-closed).
 *
 * Scenes (the build compiles each stage once, with no variant defines, so these
 * are the variants the Vulkan backend ships):
 *   - PostProcess_ACES: FullscreenQuad + PostProcess (default ACES tonemap) over
 *     a fixed HDR input.
 *   - BloomExtract: FullscreenQuad + BloomExtract over the same HDR input.
 *   - GaussianBlur_Vertical: FullscreenQuad + GaussianBlur (default vertical
 *     pass) over a fixed line pattern.
 *
 * Besides the golden comparison, every scene checks pixels against a CPU
 * evaluation of the shader formula, so a baseline that merely recorded a broken
 * render could not have been accepted in review, and every scene must finish
 * with zero validation errors.
 *
 * This is software-rasterizer shader evidence for the vulkan-lavapipe row only.
 * It is not an engine-pass golden (GraphicsEngine's passes are not involved) and
 * not hardware certification. The device must be a CPU device named llvmpipe,
 * so a hardware GPU cannot report under this row.
 *
 * Image orientation: Vulkan clip-space Y points down and FullscreenQuad does not
 * flip under VULKAN, so PNG row r is framebuffer row r and samples row r of the
 * uploaded input data.
 *
 * On a mismatch the actual frame is written to the output directory
 * (SPARK_GOLDEN_OUTPUT_DIR, default <cwd>/Tests/Output) as
 * vulkan-lavapipe_<scene>.png next to the runner's _diff.png, for review.
 */

#include "TestFramework.h"

#if defined(SPARK_VULKAN_SUPPORT) && defined(SPARK_TEST_SPIRV_DIR)

#include "Utils/GoldenImageManifest.h"
#include "Utils/GoldenImageTest.h"
#include "VulkanTestSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Spark::RHI;
using namespace SparkVkTest;

namespace
{
    constexpr const char* kRow = "vulkan-lavapipe";

    /// Mesa build the committed baselines were rendered and reviewed on. A different
    /// build is still compared (the reviewed thresholds decide); the note helps triage.
    constexpr const char* kBaselineMesa = "Mesa 25.2.8";

    /// Every scene this file certifies; must equal the manifest's entries for kRow.
    const std::array<const char*, 3> kScenes = {"PostProcess_ACES", "BloomExtract", "GaussianBlur_Vertical"};

    constexpr uint32_t kPostSize = 64;

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

    /// Requires that the device is the Lavapipe row these baselines belong to.
    void RequireLavapipe(ValidatedDevice& v)
    {
        const RHIDeviceCapabilities& caps = v.device.GetCapabilities();
        std::printf("[RHI-230 GOLDEN] row=%s device=\"%s\" api=%s software=%s\n", kRow, caps.deviceName.c_str(),
                    caps.apiVersion.c_str(), caps.isSoftwareDevice ? "yes" : "no");
        if (!caps.isSoftwareDevice || caps.deviceName.find("llvmpipe") == std::string::npos)
            SkipOrFail("vulkan-lavapipe goldens need the Lavapipe (llvmpipe) CPU device");

        VkPhysicalDeviceProperties props = {};
        vkGetPhysicalDeviceProperties(v.device.GetVkPhysicalDevice(), &props);
        const uint32_t driver = props.driverVersion;
        const std::string mesa = "Mesa " + std::to_string(VK_VERSION_MAJOR(driver)) + "." +
                                 std::to_string(VK_VERSION_MINOR(driver)) + "." +
                                 std::to_string(VK_VERSION_PATCH(driver));
        if (mesa != kBaselineMesa)
            std::printf("[RHI-230 GOLDEN] note: baselines were reviewed on %s; this run uses %s\n", kBaselineMesa,
                        mesa.c_str());
    }

    /// Hands a frame that was already read back to GoldenImageTestRunner.
    class ReadbackCapture final : public Spark::IGoldenImageCapture
    {
      public:
        ReadbackCapture(std::vector<uint8_t> pixels, uint32_t width, uint32_t height)
            : m_pixels(std::move(pixels)), m_width(width), m_height(height)
        {
        }

        std::vector<uint8_t> CaptureFramebuffer(uint32_t width, uint32_t height) override
        {
            // A size other than the rendered one fails the runner's size check.
            if (width != m_width || height != m_height)
                return {};
            return m_pixels;
        }

      private:
        std::vector<uint8_t> m_pixels;
        uint32_t m_width;
        uint32_t m_height;
    };

    /// Compares a frame with the reviewed baseline; keeps the actual frame for review on failure.
    bool MatchesGolden(const char* scene, const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height)
    {
        auto& runner = Spark::GoldenImageTestRunner::GetInstance();
        Spark::GoldenImageConfig config;
        config.goldenImageDir = GoldenDir().string();
        config.outputDir = OutputDir().string();
        config.backendRow = kRow;
        runner.Initialize(config);
        runner.SetCapture(std::make_unique<ReadbackCapture>(pixels, width, height));

        const Spark::ImageComparisonResult result = runner.CompareWithGolden(scene);
        runner.Shutdown();

        std::printf("[RHI-230 GOLDEN] scene=%s matched=%s differing=%u/%u (%.3f%%, tolerance %.3f%%) maxDist=%.2f "
                    "threshold=%.2f%s%s\n",
                    scene, result.matched ? "yes" : "no", result.differentPixels, result.totalPixels,
                    result.percentDifferent, result.tolerancePercent, result.maxPixelDistance, result.perPixelThreshold,
                    result.failureReason.empty() ? "" : " reason=", result.failureReason.c_str());

        if (!result.matched)
        {
            // The runner only keeps the actual frame on a pixel failure; keep it for every
            // failure (missing entry, hash mismatch) so a new or changed baseline can be reviewed.
            const std::filesystem::path actual = OutputDir() / (std::string(kRow) + "_" + scene + ".png");
            std::error_code ec;
            std::filesystem::create_directories(actual.parent_path(), ec);
            if (Spark::GoldenImageTestRunner::SavePNG(actual.string(), pixels.data(), width, height))
                std::printf("[RHI-230 GOLDEN] actual frame written to %s\n", actual.string().c_str());
        }
        return result.matched;
    }

    // ------------------------------------------------------------------------
    // Pixel checks
    // ------------------------------------------------------------------------

    using Color3 = std::array<float, 3>;

    std::array<int, 3> PixelAt(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t x, uint32_t y)
    {
        const size_t i = (size_t(y) * width + x) * 4;
        return {rgba[i + 0], rgba[i + 1], rgba[i + 2]};
    }

    int ToUnorm8(float value)
    {
        return static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    }

    /// Pixel (x, y) matches the expected linear-to-UNORM8 colour within `tolerance` per channel.
    bool PixelMatches(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t x, uint32_t y, const Color3& expected,
                      int tolerance = 2)
    {
        const auto px = PixelAt(rgba, width, x, y);
        bool ok = true;
        for (int c = 0; c < 3; ++c)
            ok = ok && std::abs(px[c] - ToUnorm8(expected[c])) <= tolerance;
        if (!ok)
            std::printf("[RHI-230 GOLDEN] pixel (%u,%u) = (%d,%d,%d), expected (%d,%d,%d) +/-%d\n", x, y, px[0], px[1],
                        px[2], ToUnorm8(expected[0]), ToUnorm8(expected[1]), ToUnorm8(expected[2]), tolerance);
        return ok;
    }

    // ------------------------------------------------------------------------
    // Fixed inputs
    // ------------------------------------------------------------------------

    /// HDR tile colours of the 4x4 post-process input (16x16 texels each).
    const std::array<Color3, 16> kHdrTiles = {{{0.05f, 0.05f, 0.05f},
                                               {0.25f, 0.10f, 0.05f},
                                               {0.60f, 0.30f, 0.10f},
                                               {1.20f, 0.80f, 0.20f},
                                               {0.10f, 0.20f, 0.40f},
                                               {0.50f, 0.50f, 0.50f},
                                               {1.00f, 1.00f, 1.00f},
                                               {2.00f, 1.50f, 1.00f},
                                               {0.05f, 0.40f, 0.10f},
                                               {0.80f, 0.20f, 0.60f},
                                               {1.60f, 0.40f, 0.40f},
                                               {3.00f, 3.00f, 2.50f},
                                               {0.00f, 0.00f, 0.00f},
                                               {0.30f, 0.60f, 0.90f},
                                               {2.50f, 0.50f, 2.00f},
                                               {4.00f, 3.50f, 3.00f}}};

    /// Hard-edged disc over the tiles: bright input for bloom, curved edges for the golden.
    constexpr Color3 kHdrDisc = {4.0f, 3.2f, 1.0f};
    constexpr float kDiscRadius = 12.0f;

    const Color3& HdrTile(uint32_t x, uint32_t y)
    {
        return kHdrTiles[(y / 16) * 4 + (x / 16)];
    }

    /// Texel (x, y) is data row y, column x of the upload.
    std::vector<float> MakeHdrInput()
    {
        std::vector<float> texels(size_t(kPostSize) * kPostSize * 4);
        for (uint32_t y = 0; y < kPostSize; ++y)
        {
            for (uint32_t x = 0; x < kPostSize; ++x)
            {
                const float dx = float(x) + 0.5f - 32.0f;
                const float dy = float(y) + 0.5f - 32.0f;
                const bool inDisc = dx * dx + dy * dy <= kDiscRadius * kDiscRadius;
                const Color3& color = inDisc ? kHdrDisc : HdrTile(x, y);
                float* texel = &texels[(size_t(y) * kPostSize + x) * 4];
                texel[0] = color[0];
                texel[1] = color[1];
                texel[2] = color[2];
                texel[3] = 1.0f;
            }
        }
        return texels;
    }

    constexpr uint32_t kBlurLineColumn = 20;
    constexpr uint32_t kBlurLineRow = 44;

    /// Black with a one-texel white column and row, and an orange 6x6 block.
    std::vector<uint8_t> MakeBlurInput()
    {
        std::vector<uint8_t> texels(size_t(kPostSize) * kPostSize * 4, 0);
        for (uint32_t y = 0; y < kPostSize; ++y)
        {
            for (uint32_t x = 0; x < kPostSize; ++x)
            {
                uint8_t* texel = &texels[(size_t(y) * kPostSize + x) * 4];
                texel[3] = 255;
                if (x == kBlurLineColumn || y == kBlurLineRow)
                {
                    texel[0] = texel[1] = texel[2] = 255;
                }
                else if (x >= 40 && x < 46 && y >= 8 && y < 14)
                {
                    texel[0] = 255;
                    texel[1] = 64;
                }
            }
        }
        return texels;
    }

    // ------------------------------------------------------------------------
    // CPU references of the shipped shader formulas
    // ------------------------------------------------------------------------

    /// PostProcessConstants (std140, binding 1).
    struct PostProcessConstants
    {
        float screenSize[2];
        float invScreenSize[2];
        float exposure;
        float gamma;
        float vignetteStrength;
        float vignetteRadius;
        float chromaticStrength;
        float grainStrength;
        float time;
        float saturation;
    };
    static_assert(sizeof(PostProcessConstants) == 48);

    constexpr PostProcessConstants kPostConstants = {{float(kPostSize), float(kPostSize)},
                                                     {1.0f / kPostSize, 1.0f / kPostSize},
                                                     1.0f,
                                                     2.2f,
                                                     0.0f,
                                                     0.75f,
                                                     0.0f,
                                                     0.0f,
                                                     0.0f,
                                                     0.9f};

    float ACESFilm(float x)
    {
        return std::clamp((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f);
    }

    /// PostProcess.glsl main() with no variant defines: exposure, ACES, saturation, gamma.
    Color3 PostProcessReference(const Color3& input)
    {
        Color3 color{};
        for (int c = 0; c < 3; ++c)
            color[c] = ACESFilm(input[c] * kPostConstants.exposure);
        const float luma = 0.299f * color[0] + 0.587f * color[1] + 0.114f * color[2];
        for (float& channel : color)
        {
            channel = luma + (channel - luma) * kPostConstants.saturation;
            channel = std::pow(std::max(channel, 0.0f), 1.0f / kPostConstants.gamma);
        }
        return color;
    }

    /// BloomConstants (std140, binding 1).
    struct BloomConstants
    {
        float threshold;
        float softThreshold;
        float intensity;
        float pad;
    };
    constexpr BloomConstants kBloomConstants = {1.0f, 0.5f, 0.5f, 0.0f};

    /// BloomExtract.glsl main().
    Color3 BloomReference(const Color3& input)
    {
        const float luminance = 0.299f * input[0] + 0.587f * input[1] + 0.114f * input[2];
        const float knee = kBloomConstants.threshold * kBloomConstants.softThreshold;
        float soft = std::clamp(luminance - kBloomConstants.threshold + knee, 0.0f, 2.0f * knee);
        soft = soft * soft / (4.0f * knee + 0.00001f);
        float contribution = std::max(soft, luminance - kBloomConstants.threshold);
        contribution /= std::max(luminance, 0.00001f);
        return {input[0] * contribution * kBloomConstants.intensity,
                input[1] * contribution * kBloomConstants.intensity,
                input[2] * contribution * kBloomConstants.intensity};
    }

    /// GaussianBlur.glsl weights[] for taps 0..4.
    constexpr std::array<float, 5> kBlurWeights = {0.227027f, 0.194595f, 0.121622f, 0.054054f, 0.016216f};

    // ------------------------------------------------------------------------
    // Rendering
    // ------------------------------------------------------------------------

    std::vector<uint8_t> ReadSpirv(const char* fileName)
    {
        const std::filesystem::path path = std::filesystem::path(SPARK_TEST_SPIRV_DIR) / fileName;
        std::ifstream file(path, std::ios::binary);
        std::vector<uint8_t> code{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if (code.empty())
            throw std::runtime_error("shipped SPIR-V module missing or empty: " + path.string());
        return code;
    }

    /// Owns every GPU object a test creates until the test ends, so no handle is
    /// destroyed while a submitted command buffer can still reference it.
    struct GoldenScene
    {
        ValidatedDevice& v;
        std::vector<std::unique_ptr<IRHIShader>> shaders;
        std::vector<std::unique_ptr<IRHIPipelineState>> pipelines;
        std::vector<std::unique_ptr<IRHITexture>> textures;
        std::vector<std::unique_ptr<IRHIBuffer>> buffers;
        std::vector<std::unique_ptr<IRHISampler>> samplers;

        IRHIShader* Shader(RHIShaderStage stage, const char* module)
        {
            const std::vector<uint8_t> code = ReadSpirv(module);
            RHIShaderDesc desc;
            desc.stage = stage;
            desc.language = ShaderLanguage::SPIRV;
            desc.bytecode = code.data();
            desc.bytecodeSize = code.size();
            desc.debugName = module;
            auto shader = v.device.CreateShader(desc);
            if (!shader)
                throw std::runtime_error(std::string("shipped SPIR-V rejected: ") + module);
            shaders.push_back(std::move(shader));
            return shaders.back().get();
        }

        IRHITexture* Texture(uint32_t w, uint32_t h, PixelFormat format, RHITextureUsage usage,
                             const void* data = nullptr)
        {
            RHITextureDesc desc;
            desc.width = w;
            desc.height = h;
            desc.format = format;
            desc.usage = usage;
            desc.debugName = "RHI230GoldenTexture";
            auto texture = v.device.CreateTexture(desc);
            if (!texture)
                throw std::runtime_error("texture creation failed");
            if (data)
                v.device.UpdateTexture(texture.get(), data, 0, 0);
            textures.push_back(std::move(texture));
            return textures.back().get();
        }

        IRHIBuffer* Constants(const void* data, size_t size)
        {
            RHIBufferDesc desc;
            desc.size = size;
            desc.usage = RHIBufferUsage::Constant;
            desc.access = RHIBufferAccess::Dynamic;
            desc.initialData = data;
            auto buffer = v.device.CreateBuffer(desc);
            if (!buffer)
                throw std::runtime_error("constant buffer creation failed");
            buffers.push_back(std::move(buffer));
            return buffers.back().get();
        }

        IRHISampler* Sampler(RHIFilterMode filter)
        {
            RHISamplerDesc desc;
            desc.minFilter = filter;
            desc.magFilter = filter;
            desc.mipFilter = RHIFilterMode::Nearest;
            desc.addressU = RHIAddressMode::Clamp;
            desc.addressV = RHIAddressMode::Clamp;
            desc.addressW = RHIAddressMode::Clamp;
            desc.maxAnisotropy = 1;
            auto sampler = v.device.CreateSampler(desc);
            if (!sampler)
                throw std::runtime_error("sampler creation failed");
            samplers.push_back(std::move(sampler));
            return samplers.back().get();
        }

        /// FullscreenQuad + `pixelModule` into a fresh RGBA8 target; returns the frame top-first.
        std::vector<uint8_t> FullscreenPass(const char* pixelModule, IRHITexture* input, IRHISampler* sampler,
                                            IRHIBuffer* constants)
        {
            IRHIShader* vs = Shader(RHIShaderStage::Vertex, "FullscreenQuad.vert.spv");
            IRHIShader* ps = Shader(RHIShaderStage::Pixel, pixelModule);
            RHIPipelineStateDesc desc;
            desc.numRenderTargets = 1;
            desc.renderTargetFormats[0] = PixelFormat::R8G8B8A8_UNORM;
            desc.depthStencilFormat = PixelFormat::Unknown;
            desc.depthStencil.depthEnable = false;
            desc.depthStencil.depthWrite = false;
            desc.rasterizer.cullMode = RHICullMode::None;
            desc.debugName = pixelModule;
            auto pipeline = v.device.CreatePipelineState(desc, vs, ps);
            if (!pipeline)
                throw std::runtime_error(std::string("pipeline creation failed: ") + pixelModule);
            pipelines.push_back(std::move(pipeline));

            IRHITexture* target =
                Texture(kPostSize, kPostSize, PixelFormat::R8G8B8A8_UNORM,
                        RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource | RHITextureUsage::TransferSrc);
            IRHICommandList* cmd = v.device.GetImmediateCommandList();
            IRHITexture* targets[] = {target};
            // Magenta: any pixel the pass fails to cover stands out in the golden.
            const float magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
            cmd->Begin();
            cmd->ClearRenderTarget(target, magenta);
            // The sampled-image layout barrier cannot be recorded inside dynamic rendering,
            // so the input is bound before the render target opens the rendering scope.
            cmd->SetShaderResource(RHIShaderStage::Pixel, 0, input);
            cmd->SetSampler(RHIShaderStage::Pixel, 0, sampler);
            cmd->SetRenderTargets(targets, 1, nullptr);
            RHIViewport viewport;
            viewport.width = float(kPostSize);
            viewport.height = float(kPostSize);
            cmd->SetViewport(viewport);
            RHIScissorRect scissor;
            scissor.right = static_cast<int32_t>(kPostSize);
            scissor.bottom = static_cast<int32_t>(kPostSize);
            cmd->SetScissorRect(scissor);
            cmd->SetPipelineState(pipelines.back().get());
            cmd->SetConstantBuffer(RHIShaderStage::Pixel, 1, constants);
            cmd->Draw(3, 0);
            cmd->End();
            v.device.ExecuteCommandList(cmd);
            v.device.WaitForIdle();
            return v.device.ReadbackTexture(target);
        }
    };

    /// Tile centres of the four corner tiles, far from the disc and tile edges.
    constexpr std::array<std::array<uint32_t, 2>, 4> kFlatProbes = {{{8, 8}, {56, 8}, {8, 56}, {56, 56}}};
} // namespace

// ----------------------------------------------------------------------------
// The manifest lists exactly this file's scenes for the row: no orphaned
// baseline and no scene that could lose its reviewed entry unnoticed.
// ----------------------------------------------------------------------------
TEST(VulkanGolden_RHI230_ManifestCoversRowScenes)
{
    std::vector<Spark::GoldenManifestEntry> entries;
    std::string error;
    ASSERT_TRUE(Spark::GoldenManifest::Load(GoldenDir() / "manifest.json", entries, error));

    std::set<std::string> manifestScenes;
    for (const auto& entry : entries)
    {
        if (entry.backendRow != kRow)
            continue;
        manifestScenes.insert(entry.scene);
        EXPECT_TRUE(entry.software);
        EXPECT_TRUE(std::filesystem::is_regular_file(GoldenDir() / kRow / (entry.scene + ".png")));
    }
    const std::set<std::string> expected(kScenes.begin(), kScenes.end());
    for (const auto& scene : manifestScenes)
    {
        if (!expected.contains(scene))
            std::printf("[RHI-230 GOLDEN] manifest entry without a test: %s\n", scene.c_str());
    }
    for (const auto& scene : expected)
    {
        if (!manifestScenes.contains(scene))
            std::printf("[RHI-230 GOLDEN] scene without a manifest entry: %s\n", scene.c_str());
    }
    EXPECT_TRUE(manifestScenes == expected);
}

// ----------------------------------------------------------------------------
// FullscreenQuad + PostProcess: the shipped (ACES) tonemap variant.
// ----------------------------------------------------------------------------
TEST(VulkanGolden_RHI230_PostProcessACES)
{
    ValidatedDevice v("RHI230Golden");
    RequireLavapipe(v);
    {
        GoldenScene golden{v};
        const std::vector<float> hdr = MakeHdrInput();
        IRHITexture* input = golden.Texture(kPostSize, kPostSize, PixelFormat::R32G32B32A32_FLOAT,
                                            RHITextureUsage::ShaderResource, hdr.data());
        IRHISampler* linearClamp = golden.Sampler(RHIFilterMode::Linear);
        IRHIBuffer* constants = golden.Constants(&kPostConstants, sizeof(kPostConstants));

        const auto frame = golden.FullscreenPass("PostProcess.frag.spv", input, linearClamp, constants);
        ASSERT_EQ(frame.size(), size_t(kPostSize) * kPostSize * 4);
        EXPECT_TRUE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(frame, 0.5));

        for (const auto& probe : kFlatProbes)
        {
            EXPECT_TRUE(
                PixelMatches(frame, kPostSize, probe[0], probe[1], PostProcessReference(HdrTile(probe[0], probe[1]))));
        }
        EXPECT_TRUE(PixelMatches(frame, kPostSize, 32, 32, PostProcessReference(kHdrDisc)));

        EXPECT_TRUE(MatchesGolden("PostProcess_ACES", frame, kPostSize, kPostSize));
    }
    v.ExpectClean();
}

// ----------------------------------------------------------------------------
// FullscreenQuad + BloomExtract: soft-knee bright pass over the HDR input.
// ----------------------------------------------------------------------------
TEST(VulkanGolden_RHI230_BloomExtract)
{
    ValidatedDevice v("RHI230Golden");
    RequireLavapipe(v);
    {
        GoldenScene golden{v};
        const std::vector<float> hdr = MakeHdrInput();
        IRHITexture* input = golden.Texture(kPostSize, kPostSize, PixelFormat::R32G32B32A32_FLOAT,
                                            RHITextureUsage::ShaderResource, hdr.data());
        IRHISampler* pointClamp = golden.Sampler(RHIFilterMode::Nearest);
        IRHIBuffer* constants = golden.Constants(&kBloomConstants, sizeof(kBloomConstants));

        const auto frame = golden.FullscreenPass("BloomExtract.frag.spv", input, pointClamp, constants);
        ASSERT_EQ(frame.size(), size_t(kPostSize) * kPostSize * 4);
        EXPECT_TRUE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(frame, 0.9));

        for (const auto& probe : kFlatProbes)
        {
            EXPECT_TRUE(
                PixelMatches(frame, kPostSize, probe[0], probe[1], BloomReference(HdrTile(probe[0], probe[1]))));
        }
        EXPECT_TRUE(PixelMatches(frame, kPostSize, 32, 32, BloomReference(kHdrDisc)));
        // Dark tiles are below the knee and extract to black.
        EXPECT_TRUE(PixelMatches(frame, kPostSize, 8, 8, {0, 0, 0}, 0));

        EXPECT_TRUE(MatchesGolden("BloomExtract", frame, kPostSize, kPostSize));
    }
    v.ExpectClean();
}

// ----------------------------------------------------------------------------
// FullscreenQuad + GaussianBlur: the shipped (vertical) 9-tap pass.
// ----------------------------------------------------------------------------
TEST(VulkanGolden_RHI230_GaussianBlurVertical)
{
    ValidatedDevice v("RHI230Golden");
    RequireLavapipe(v);
    {
        GoldenScene golden{v};
        const std::vector<uint8_t> lines = MakeBlurInput();
        IRHITexture* input = golden.Texture(kPostSize, kPostSize, PixelFormat::R8G8B8A8_UNORM,
                                            RHITextureUsage::ShaderResource, lines.data());
        // Point sampling keeps every tap on a texel centre, so the CPU weights are exact.
        IRHISampler* pointClamp = golden.Sampler(RHIFilterMode::Nearest);
        const float texelSize[4] = {1.0f / kPostSize, 1.0f / kPostSize, 0.0f, 0.0f};
        IRHIBuffer* constants = golden.Constants(texelSize, sizeof(texelSize));

        const auto frame = golden.FullscreenPass("GaussianBlur.frag.spv", input, pointClamp, constants);
        ASSERT_EQ(frame.size(), size_t(kPostSize) * kPostSize * 4);

        // The white row spreads by the tap weights down the column; the white
        // column is unchanged (the weights sum to 1).
        for (uint32_t tap = 0; tap < 5; ++tap)
        {
            const float w = kBlurWeights[tap];
            EXPECT_TRUE(PixelMatches(frame, kPostSize, 5, kBlurLineRow + tap, {w, w, w}, 1));
            EXPECT_TRUE(PixelMatches(frame, kPostSize, 5, kBlurLineRow - tap, {w, w, w}, 1));
        }
        EXPECT_TRUE(PixelMatches(frame, kPostSize, 5, kBlurLineRow + 5, {0, 0, 0}, 0));
        EXPECT_TRUE(PixelMatches(frame, kPostSize, kBlurLineColumn, 30, {1, 1, 1}, 1));

        EXPECT_TRUE(MatchesGolden("GaussianBlur_Vertical", frame, kPostSize, kPostSize));
    }
    v.ExpectClean();
}

#endif // SPARK_VULKAN_SUPPORT && SPARK_TEST_SPIRV_DIR
