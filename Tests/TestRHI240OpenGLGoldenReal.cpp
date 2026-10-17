/**
 * @file TestRHI240OpenGLGoldenReal.cpp
 * @brief RHI-240: shipped-shader OpenGL goldens on the opengl-llvmpipe row.
 *
 * Each test renders a deterministic scene through a real GLDevice with the
 * production shaders in Shaders/GLSL, reads the render target back, and
 * compares it with the committed baseline under
 * Tests/GoldenImages/opengl-llvmpipe/ using the reviewed thresholds and
 * baseline SHA-256 in Tests/GoldenImages/manifest.json
 * (GoldenImageTestRunner, fail-closed).
 *
 * Scenes:
 *   - LitSphere_BasicVS_BasicPS: BasicVS + BasicPS lighting a UV sphere with a
 *     checker albedo, flat normal map, directional light and ambient term.
 *   - PostProcess_ACES / _Reinhard / _Uncharted2 / _FXAA: FullscreenQuad +
 *     PostProcess over a fixed HDR input (tonemap variants, and FXAA with the
 *     default ACES tonemap).
 *   - GaussianBlur_Horizontal / _Vertical: FullscreenQuad + GaussianBlur over
 *     a fixed line pattern.
 *   - BloomExtract: FullscreenQuad + BloomExtract over the HDR input.
 *
 * Besides the golden comparison, every scene checks a few pixels against a CPU
 * evaluation of the shader formula (or the scene geometry), so a baseline that
 * merely recorded a broken render could not have been accepted in review.
 *
 * This is software-rasterizer shader evidence for the opengl-llvmpipe row only.
 * It is not an engine-pass golden (GraphicsEngine's Linux passes are not
 * involved) and not hardware driver certification. A non-llvmpipe context
 * skips the scenes, and fails them under SPARK_REQUIRE_OPENGL=1 (the dedicated
 * SparkOpenGLGoldenTests lane), so a hardware context cannot report under this row.
 *
 * Image orientation: PNG row 0 is the top of the rendered frame (GL row H-1).
 * With the GL FullscreenQuad Y flip, PNG row r of a fullscreen pass samples
 * row r of the uploaded input data.
 *
 * On a mismatch the actual frame is written to the output directory
 * (SPARK_GOLDEN_OUTPUT_DIR, default <cwd>/Tests/Output) as
 * opengl-llvmpipe_<scene>.png next to the runner's _diff.png, for review.
 */

#include "TestFramework.h"

#ifdef SPARK_OPENGL_SUPPORT

#include "OpenGLTestSupport.h"
#include "Utils/GoldenImageManifest.h"
#include "Utils/GoldenImageTest.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Spark::RHI;
using namespace SparkGLTest;

namespace
{
    constexpr const char* kRow = "opengl-llvmpipe";

    /// Mesa build the committed baselines were rendered and reviewed on. A different
    /// build is still compared (the reviewed thresholds decide); the note helps triage.
    constexpr const char* kBaselineMesa = "Mesa 25.2.8";

    /// Every scene this file certifies; must equal the manifest's entries for kRow.
    const std::array<const char*, 8> kScenes = {
        "LitSphere_BasicVS_BasicPS", "PostProcess_ACES",        "PostProcess_Reinhard",  "PostProcess_Uncharted2",
        "PostProcess_FXAA",          "GaussianBlur_Horizontal", "GaussianBlur_Vertical", "BloomExtract"};

    constexpr uint32_t kPostSize = 64;
    constexpr uint32_t kLitSize = 96;

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

    /// Requires a GL context on the llvmpipe row these baselines belong to; skips (or fails
    /// under SPARK_REQUIRE_OPENGL=1) otherwise.
    void RequireLlvmpipe(GLTestDevice& gl)
    {
        RequireGL(gl);
        const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const std::string rendererText = renderer ? renderer : "";
        const std::string versionText = version ? version : "";
        std::printf("[RHI-240 GOLDEN] row=%s GL_RENDERER=\"%s\" GL_VERSION=\"%s\"\n", kRow, rendererText.c_str(),
                    versionText.c_str());
        if (rendererText.find("llvmpipe") == std::string::npos || !gl.device.GetCapabilities().isSoftwareDevice)
        {
            // A hardware context is a different row, not a defect: outside the dedicated lane
            // (e.g. a Windows or Linux desktop with a GPU) the scene is skipped. The lane sets
            // SPARK_REQUIRE_OPENGL=1, so there a non-llvmpipe context fails instead of passing by skip.
            const char* required = std::getenv("SPARK_REQUIRE_OPENGL");
            if (required != nullptr && std::string(required) == "1")
                throw std::runtime_error("SPARK_REQUIRE_OPENGL=1 but the context is not llvmpipe, got GL_RENDERER=\"" +
                                         rendererText + "\"");
            SKIP_TEST("opengl-llvmpipe goldens need an llvmpipe context");
        }
        if (versionText.find(kBaselineMesa) == std::string::npos)
            std::printf("[RHI-240 GOLDEN] note: baselines were reviewed on %s; this run uses \"%s\"\n", kBaselineMesa,
                        versionText.c_str());
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

        std::printf("[RHI-240 GOLDEN] scene=%s matched=%s differing=%u/%u (%.3f%%, tolerance %.3f%%) maxDist=%.2f "
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
                std::printf("[RHI-240 GOLDEN] actual frame written to %s\n", actual.string().c_str());
        }
        return result.matched;
    }

    // ------------------------------------------------------------------------
    // Readback and pixel checks
    // ------------------------------------------------------------------------

    /// Reads an RGBA8 render target and returns rows top-first (PNG order).
    std::vector<uint8_t> ReadbackTopDown(IRHITexture* texture, uint32_t width, uint32_t height)
    {
        const GLuint name = static_cast<GLuint>(reinterpret_cast<uintptr_t>(texture->GetNativeHandle()));
        const size_t rowBytes = size_t(width) * 4;
        std::vector<uint8_t> bottomUp(rowBytes * height);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTextureImage(name, 0, GL_RGBA, GL_UNSIGNED_BYTE, static_cast<GLsizei>(bottomUp.size()), bottomUp.data());

        std::vector<uint8_t> topDown(bottomUp.size());
        for (uint32_t row = 0; row < height; ++row)
        {
            std::copy_n(bottomUp.begin() + static_cast<std::ptrdiff_t>((height - 1 - row) * rowBytes), rowBytes,
                        topDown.begin() + static_cast<std::ptrdiff_t>(row * rowBytes));
        }
        return topDown;
    }

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
    bool PixelMatches(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t x, uint32_t y,
                      const std::array<float, 3>& expected, int tolerance = 2)
    {
        const auto px = PixelAt(rgba, width, x, y);
        bool ok = true;
        for (int c = 0; c < 3; ++c)
            ok = ok && std::abs(px[c] - ToUnorm8(expected[c])) <= tolerance;
        if (!ok)
            std::printf("[RHI-240 GOLDEN] pixel (%u,%u) = (%d,%d,%d), expected (%d,%d,%d) +/-%d\n", x, y, px[0], px[1],
                        px[2], ToUnorm8(expected[0]), ToUnorm8(expected[1]), ToUnorm8(expected[2]), tolerance);
        return ok;
    }

    float Luma(const std::array<int, 3>& px)
    {
        return 0.299f * float(px[0]) + 0.587f * float(px[1]) + 0.114f * float(px[2]);
    }

    // ------------------------------------------------------------------------
    // Fixed inputs
    // ------------------------------------------------------------------------

    using Color3 = std::array<float, 3>;

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

    /// Hard-edged disc over the tiles: diagonal edges for FXAA, bright input for bloom.
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

    enum class Tonemap
    {
        ACES,
        Reinhard,
        Uncharted2
    };

    float ACESFilm(float x)
    {
        return std::clamp((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f);
    }

    float Uncharted2(float x)
    {
        constexpr float A = 0.15f, B = 0.50f, C = 0.10f, D = 0.20f, E = 0.02f, F = 0.30f;
        return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
    }

    /// PostProcess.glsl main() for a flat input region (no vignette/grain/chromatic).
    Color3 PostProcessReference(const Color3& input, Tonemap tonemap)
    {
        Color3 color{};
        for (int c = 0; c < 3; ++c)
        {
            const float x = input[c] * kPostConstants.exposure;
            switch (tonemap)
            {
            case Tonemap::ACES:
                color[c] = ACESFilm(x);
                break;
            case Tonemap::Reinhard:
                color[c] = x / (x + 1.0f);
                break;
            case Tonemap::Uncharted2:
                color[c] = Uncharted2(x * 2.0f) / Uncharted2(11.2f);
                break;
            }
        }
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

    /// Owns every GPU object a test creates until the test ends. Pipelines in
    /// particular stay alive: GLCommandList skips a SetPipelineState whose pointer
    /// equals the last one bound, so a freed pipeline's address must not be reused.
    struct GoldenScene
    {
        GLTestDevice& gl;
        std::vector<std::unique_ptr<IRHIShader>> shaders;
        std::vector<std::unique_ptr<IRHIPipelineState>> pipelines;
        std::vector<std::unique_ptr<IRHITexture>> textures;
        std::vector<std::unique_ptr<IRHIBuffer>> buffers;
        std::vector<std::unique_ptr<IRHISampler>> samplers;

        IRHIShader* Shader(RHIShaderStage stage, const char* file, const std::vector<std::string>& defines)
        {
            auto shader = MakeShader(gl.device, stage, ReadFile(GLSLDir() / file), defines);
            if (!shader)
                throw std::runtime_error(std::string("shipped shader failed to compile: ") + file);
            shaders.push_back(std::move(shader));
            return shaders.back().get();
        }

        IRHIPipelineState* Pipeline(const RHIPipelineStateDesc& desc, IRHIShader* vs, IRHIShader* ps)
        {
            auto pipeline = gl.device.CreatePipelineState(desc, vs, ps);
            if (!pipeline)
                throw std::runtime_error("pipeline creation failed");
            pipelines.push_back(std::move(pipeline));
            return pipelines.back().get();
        }

        IRHITexture* Texture(uint32_t w, uint32_t h, PixelFormat format, RHITextureUsage usage,
                             const void* data = nullptr)
        {
            auto texture = MakeTexture(gl.device, w, h, format, usage);
            if (!texture)
                throw std::runtime_error("texture creation failed");
            if (data)
                gl.device.UpdateTexture(texture.get(), data, 0, 0);
            textures.push_back(std::move(texture));
            return textures.back().get();
        }

        IRHITexture* Solid1x1(uint8_t r, uint8_t g, uint8_t b)
        {
            const uint8_t texel[4] = {r, g, b, 255};
            return Texture(1, 1, PixelFormat::R8G8B8A8_UNORM, RHITextureUsage::ShaderResource, texel);
        }

        IRHIBuffer* Constants(const void* data, size_t size)
        {
            RHIBufferDesc desc;
            desc.size = size;
            desc.usage = RHIBufferUsage::Constant;
            desc.initialData = data;
            auto buffer = gl.device.CreateBuffer(desc);
            if (!buffer)
                throw std::runtime_error("constant buffer creation failed");
            buffers.push_back(std::move(buffer));
            return buffers.back().get();
        }

        IRHISampler* Sampler(RHIFilterMode filter, RHIAddressMode address)
        {
            RHISamplerDesc desc;
            desc.minFilter = filter;
            desc.magFilter = filter;
            desc.mipFilter = RHIFilterMode::Nearest;
            desc.addressU = address;
            desc.addressV = address;
            desc.addressW = address;
            desc.maxAnisotropy = 1;
            auto sampler = gl.device.CreateSampler(desc);
            if (!sampler)
                throw std::runtime_error("sampler creation failed");
            samplers.push_back(std::move(sampler));
            return samplers.back().get();
        }

        /// FullscreenQuad + `psFile` into a fresh RGBA8 target; returns the frame top-first.
        std::vector<uint8_t> FullscreenPass(const char* psFile, const std::vector<std::string>& defines,
                                            IRHITexture* input, IRHISampler* sampler, IRHIBuffer* constants)
        {
            IRHIShader* vs = Shader(RHIShaderStage::Vertex, "FullscreenQuad.glsl", defines);
            IRHIShader* ps = Shader(RHIShaderStage::Pixel, psFile, defines);
            RHIPipelineStateDesc desc;
            desc.rasterizer.cullMode = RHICullMode::None;
            desc.depthStencil.depthEnable = false;
            desc.depthStencil.depthWrite = false;
            IRHIPipelineState* pipeline = Pipeline(desc, vs, ps);

            IRHITexture* target = Texture(kPostSize, kPostSize, PixelFormat::R8G8B8A8_UNORM,
                                          RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource);
            IRHICommandList* cmd = gl.device.GetImmediateCommandList();
            IRHITexture* targets[] = {target};
            cmd->SetRenderTargets(targets, 1, nullptr);
            // Magenta: any pixel the pass fails to cover stands out in the golden.
            const float magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
            cmd->ClearRenderTarget(target, magenta);
            cmd->SetViewport({0, 0, float(kPostSize), float(kPostSize), 0, 1});
            cmd->SetPipelineState(pipeline);
            cmd->SetPrimitiveTopology(RHIPrimitiveTopology::TriangleList);
            cmd->SetConstantBuffer(RHIShaderStage::Pixel, 1, constants);
            cmd->SetShaderResource(RHIShaderStage::Pixel, 0, input);
            cmd->SetSampler(RHIShaderStage::Pixel, 0, sampler);
            cmd->Draw(3, 0);
            gl.device.WaitForIdle();
            return ReadbackTopDown(target, kPostSize, kPostSize);
        }
    };

    /// Tile centres of the four corner tiles, far from the disc and tile edges.
    constexpr std::array<std::array<uint32_t, 2>, 4> kFlatProbes = {{{8, 8}, {56, 8}, {8, 56}, {56, 56}}};

    void CheckTonemapScene(const char* scene, Tonemap tonemap, const std::vector<std::string>& defines)
    {
        GLTestDevice gl;
        RequireLlvmpipe(gl);
        GoldenScene golden{gl};

        const std::vector<float> hdr = MakeHdrInput();
        IRHITexture* input = golden.Texture(kPostSize, kPostSize, PixelFormat::R32G32B32A32_FLOAT,
                                            RHITextureUsage::ShaderResource, hdr.data());
        IRHISampler* linearClamp = golden.Sampler(RHIFilterMode::Linear, RHIAddressMode::Clamp);
        IRHIBuffer* constants = golden.Constants(&kPostConstants, sizeof(kPostConstants));

        const auto frame = golden.FullscreenPass("PostProcess.glsl", defines, input, linearClamp, constants);
        ASSERT_EQ(frame.size(), size_t(kPostSize) * kPostSize * 4);
        EXPECT_TRUE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(frame, 0.5));

        // Flat regions: FXAA returns the centre sample there, so every variant equals the formula.
        for (const auto& probe : kFlatProbes)
        {
            EXPECT_TRUE(PixelMatches(frame, kPostSize, probe[0], probe[1],
                                     PostProcessReference(HdrTile(probe[0], probe[1]), tonemap)));
        }
        EXPECT_TRUE(PixelMatches(frame, kPostSize, 32, 32, PostProcessReference(kHdrDisc, tonemap)));

        EXPECT_TRUE(MatchesGolden(scene, frame, kPostSize, kPostSize));
        EXPECT_EQ(gl.Errors(), 0);
    }

    // ------------------------------------------------------------------------
    // Lit sphere (BasicVS + BasicPS)
    // ------------------------------------------------------------------------

    /// Column-major 4x4 matrix, as GLSL std140 mat4 expects.
    using Mat4 = std::array<float, 16>;

    Mat4 Multiply(const Mat4& a, const Mat4& b)
    {
        Mat4 r{};
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
            {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k)
                    sum += a[k * 4 + row] * b[col * 4 + k];
                r[col * 4 + row] = sum;
            }
        return r;
    }

    Mat4 RotationY(float radians)
    {
        const float c = std::cos(radians);
        const float s = std::sin(radians);
        return {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1};
    }

    /// Right-handed view looking from `eye` down -Z at the origin (eye on the +Z axis).
    Mat4 ViewFromPositiveZ(float eyeZ)
    {
        return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -eyeZ, 1};
    }

    /// OpenGL perspective projection (clip z in [-w, w]).
    Mat4 Perspective(float fovY, float aspect, float nearZ, float farZ)
    {
        const float f = 1.0f / std::tan(fovY * 0.5f);
        Mat4 m{};
        m[0] = f / aspect;
        m[5] = f;
        m[10] = (farZ + nearZ) / (nearZ - farZ);
        m[11] = -1.0f;
        m[14] = (2.0f * farZ * nearZ) / (nearZ - farZ);
        return m;
    }

    /// BasicVS/BasicPS PerFrameConstants (std140, binding 0).
    struct PerFrameConstants
    {
        Mat4 view;
        Mat4 projection;
        Mat4 viewProjection;
        float cameraPosition[3];
        float time;
        float cameraDirection[3];
        float deltaTime;
        float screenResolution[2];
        float invScreenResolution[2];
        float lightDirection[3];
        float lightIntensity;
        float lightColor[3];
        float ambientIntensity;
        float ambientColor[3];
        float padding;
    };
    static_assert(sizeof(PerFrameConstants) == 288);

    /// BasicVS/BasicPS PerObjectConstants (std140, binding 1).
    struct PerObjectConstants
    {
        Mat4 world;
        Mat4 worldViewProjection;
        Mat4 worldInverseTranspose;
        Mat4 previousWorld;
        float objectPosition[3];
        float objectScale;
        float objectColor[4];
        float materialProperties[4];
        float uvTiling[4];
    };
    static_assert(sizeof(PerObjectConstants) == 320);

    /// BasicPS PerMaterialConstants (std140, binding 2).
    struct PerMaterialConstants
    {
        float albedoColor[4];
        float metallicFactor;
        float roughnessFactor;
        float normalScale;
        float occlusionStrength;
        float emissiveFactor;
        float alphaCutoff;
        float padding[2];
    };
    static_assert(sizeof(PerMaterialConstants) == 48);

    struct LitVertex
    {
        float position[3];
        float normal[3];
        float uv[2];
    };

    /// Unit UV sphere, `segments` around and `rings` pole to pole, indexed triangle list.
    void BuildSphere(uint32_t segments, uint32_t rings, std::vector<LitVertex>& vertices,
                     std::vector<uint32_t>& indices)
    {
        constexpr float kPi = 3.14159265358979f;
        for (uint32_t ring = 0; ring <= rings; ++ring)
        {
            const float v = float(ring) / float(rings);
            const float theta = v * kPi;
            for (uint32_t segment = 0; segment <= segments; ++segment)
            {
                const float u = float(segment) / float(segments);
                const float phi = u * 2.0f * kPi;
                const float x = std::sin(theta) * std::cos(phi);
                const float y = std::cos(theta);
                const float z = std::sin(theta) * std::sin(phi);
                vertices.push_back({{x, y, z}, {x, y, z}, {u, v}});
            }
        }
        const uint32_t stride = segments + 1;
        for (uint32_t ring = 0; ring < rings; ++ring)
        {
            for (uint32_t segment = 0; segment < segments; ++segment)
            {
                const uint32_t a = ring * stride + segment;
                const uint32_t b = a + stride;
                indices.insert(indices.end(), {a, b, a + 1, a + 1, b, b + 1});
            }
        }
    }
} // namespace

// ----------------------------------------------------------------------------
// The manifest lists exactly this file's scenes for the row: no orphaned
// baseline and no scene that could lose its reviewed entry unnoticed.
// ----------------------------------------------------------------------------
TEST(OpenGLGolden_RHI240_ManifestCoversRowScenes)
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
    if (manifestScenes != expected)
    {
        for (const auto& scene : manifestScenes)
            if (!expected.contains(scene))
                std::printf("[RHI-240 GOLDEN] manifest entry without a test: %s\n", scene.c_str());
        for (const auto& scene : expected)
            if (!manifestScenes.contains(scene))
                std::printf("[RHI-240 GOLDEN] scene without a manifest entry: %s\n", scene.c_str());
    }
    EXPECT_TRUE(manifestScenes == expected);
}

// ----------------------------------------------------------------------------
// BasicVS + BasicPS: lit, textured sphere with depth testing.
// ----------------------------------------------------------------------------
TEST(OpenGLGolden_RHI240_LitSphereBasicShaders)
{
    GLTestDevice gl;
    RequireLlvmpipe(gl);
    GoldenScene golden{gl};

    constexpr float kEyeZ = 3.0f;
    const Mat4 world = RotationY(0.6f);
    const Mat4 view = ViewFromPositiveZ(kEyeZ);
    const Mat4 projection = Perspective(0.7853982f, 1.0f, 0.1f, 10.0f);
    const Mat4 viewProjection = Multiply(projection, view);

    PerFrameConstants frame{};
    frame.view = view;
    frame.projection = projection;
    frame.viewProjection = viewProjection;
    frame.cameraPosition[2] = kEyeZ;
    frame.cameraDirection[2] = -1.0f;
    frame.screenResolution[0] = frame.screenResolution[1] = float(kLitSize);
    frame.invScreenResolution[0] = frame.invScreenResolution[1] = 1.0f / kLitSize;
    // Light travels down-left-back, so the upper-right-front of the sphere is lit.
    const float lightLength = std::sqrt(0.5f * 0.5f + 0.7f * 0.7f + 0.5f * 0.5f);
    frame.lightDirection[0] = -0.5f / lightLength;
    frame.lightDirection[1] = -0.7f / lightLength;
    frame.lightDirection[2] = -0.5f / lightLength;
    frame.lightIntensity = 3.0f;
    frame.lightColor[0] = 1.0f;
    frame.lightColor[1] = 0.95f;
    frame.lightColor[2] = 0.9f;
    frame.ambientIntensity = 1.0f;
    frame.ambientColor[0] = 0.25f;
    frame.ambientColor[1] = 0.3f;
    frame.ambientColor[2] = 0.4f;

    PerObjectConstants object{};
    object.world = world;
    object.worldViewProjection = Multiply(viewProjection, world);
    object.worldInverseTranspose = world; // pure rotation
    object.previousWorld = world;
    object.objectScale = 1.0f;
    std::fill(std::begin(object.objectColor), std::end(object.objectColor), 1.0f);
    object.materialProperties[0] = 0.2f;  // metallic
    object.materialProperties[1] = 0.45f; // roughness
    object.materialProperties[2] = 1.0f;  // emissive scale
    object.materialProperties[3] = 1.0f;
    object.uvTiling[0] = 2.0f;
    object.uvTiling[1] = 1.0f;

    PerMaterialConstants material{};
    std::fill(std::begin(material.albedoColor), std::end(material.albedoColor), 1.0f);
    material.metallicFactor = 1.0f;
    material.roughnessFactor = 1.0f;
    material.normalScale = 1.0f;
    material.occlusionStrength = 1.0f;
    material.emissiveFactor = 0.5f;
    material.alphaCutoff = 0.0f;

    // 8x8 two-colour checker albedo.
    std::vector<uint8_t> checker(8 * 8 * 4);
    for (uint32_t y = 0; y < 8; ++y)
    {
        for (uint32_t x = 0; x < 8; ++x)
        {
            const bool odd = ((x + y) & 1u) != 0;
            uint8_t* texel = &checker[(y * 8 + x) * 4];
            texel[0] = odd ? 60 : 230;
            texel[1] = odd ? 110 : 120;
            texel[2] = odd ? 200 : 40;
            texel[3] = 255;
        }
    }

    std::vector<LitVertex> vertices;
    std::vector<uint32_t> indices;
    BuildSphere(32, 16, vertices, indices);

    RHIBufferDesc vbDesc;
    vbDesc.size = vertices.size() * sizeof(LitVertex);
    vbDesc.stride = sizeof(LitVertex);
    vbDesc.usage = RHIBufferUsage::Vertex;
    vbDesc.initialData = vertices.data();
    auto vb = gl.device.CreateBuffer(vbDesc);
    RHIBufferDesc ibDesc;
    ibDesc.size = indices.size() * sizeof(uint32_t);
    ibDesc.stride = sizeof(uint32_t);
    ibDesc.usage = RHIBufferUsage::Index;
    ibDesc.initialData = indices.data();
    auto ib = gl.device.CreateBuffer(ibDesc);
    ASSERT_TRUE(vb != nullptr && ib != nullptr);

    IRHIShader* vs = golden.Shader(RHIShaderStage::Vertex, "BasicVS.glsl", {});
    IRHIShader* ps = golden.Shader(RHIShaderStage::Pixel, "BasicPS.glsl", {});
    RHIPipelineStateDesc desc;
    desc.inputLayout.elements.push_back({"POSITION", 0, RHIVertexFormat::Float3, 0, 0, false, 0});
    desc.inputLayout.elements.push_back({"NORMAL", 0, RHIVertexFormat::Float3, 0, 12, false, 0});
    desc.inputLayout.elements.push_back({"TEXCOORD", 0, RHIVertexFormat::Float2, 0, 24, false, 0});
    desc.rasterizer.cullMode = RHICullMode::None; // depth testing resolves the far side
    IRHIPipelineState* pipeline = golden.Pipeline(desc, vs, ps);

    IRHITexture* target = golden.Texture(kLitSize, kLitSize, PixelFormat::R8G8B8A8_UNORM,
                                         RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource);
    IRHITexture* depth = golden.Texture(kLitSize, kLitSize, PixelFormat::D32_FLOAT, RHITextureUsage::DepthStencil);
    IRHITexture* albedo =
        golden.Texture(8, 8, PixelFormat::R8G8B8A8_UNORM, RHITextureUsage::ShaderResource, checker.data());
    IRHITexture* normal = golden.Solid1x1(128, 128, 255);
    IRHITexture* metallicRoughness = golden.Solid1x1(255, 255, 255);
    IRHITexture* occlusion = golden.Solid1x1(255, 255, 255);
    IRHITexture* emissive = golden.Solid1x1(40, 10, 0);
    IRHISampler* linearWrap = golden.Sampler(RHIFilterMode::Linear, RHIAddressMode::Wrap);

    IRHIBuffer* frameCB = golden.Constants(&frame, sizeof(frame));
    IRHIBuffer* objectCB = golden.Constants(&object, sizeof(object));
    IRHIBuffer* materialCB = golden.Constants(&material, sizeof(material));

    IRHICommandList* cmd = gl.device.GetImmediateCommandList();
    IRHITexture* targets[] = {target};
    cmd->SetRenderTargets(targets, 1, depth);
    const float background[4] = {0.05f, 0.05f, 0.08f, 1.0f};
    cmd->ClearRenderTarget(target, background);
    cmd->ClearDepthStencil(depth, 1.0f, 0);
    cmd->SetViewport({0, 0, float(kLitSize), float(kLitSize), 0, 1});
    cmd->SetPipelineState(pipeline);
    cmd->SetPrimitiveTopology(RHIPrimitiveTopology::TriangleList);
    cmd->SetVertexBuffer(vb.get(), 0, 0);
    cmd->SetIndexBuffer(ib.get(), 0);
    cmd->SetConstantBuffer(RHIShaderStage::Vertex, 0, frameCB);
    cmd->SetConstantBuffer(RHIShaderStage::Vertex, 1, objectCB);
    cmd->SetConstantBuffer(RHIShaderStage::Pixel, 2, materialCB);
    IRHITexture* textures[] = {albedo, normal, metallicRoughness, occlusion, emissive};
    for (uint32_t slot = 0; slot < 5; ++slot)
    {
        cmd->SetShaderResource(RHIShaderStage::Pixel, slot, textures[slot]);
        cmd->SetSampler(RHIShaderStage::Pixel, slot, linearWrap);
    }
    cmd->DrawIndexed(static_cast<uint32_t>(indices.size()), 0, 0);
    gl.device.WaitForIdle();

    const auto pixels = ReadbackTopDown(target, kLitSize, kLitSize);
    ASSERT_EQ(pixels.size(), size_t(kLitSize) * kLitSize * 4);
    EXPECT_TRUE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(pixels, 0.7));

    // Geometry: the unit sphere at distance 3 under a 45-degree FOV spans ~74% of
    // the frame height, so the corners are background and the centre is sphere.
    EXPECT_TRUE(PixelMatches(pixels, kLitSize, 1, 1, {0.05f, 0.05f, 0.08f}, 1));
    EXPECT_TRUE(PixelMatches(pixels, kLitSize, kLitSize - 2, kLitSize - 2, {0.05f, 0.05f, 0.08f}, 1));
    const auto centre = PixelAt(pixels, kLitSize, kLitSize / 2, kLitSize / 2);
    EXPECT_TRUE(centre != (std::array<int, 3>{13, 13, 20}));

    // Lighting: the side facing the light (upper right) is brighter than the far side (lower left).
    const float litLuma = Luma(PixelAt(pixels, kLitSize, 62, 34));
    const float shadowLuma = Luma(PixelAt(pixels, kLitSize, 34, 62));
    std::printf("[RHI-240 GOLDEN] lit sphere luma: lit side %.1f, far side %.1f\n", litLuma, shadowLuma);
    EXPECT_GT(litLuma, shadowLuma + 40.0f);

    EXPECT_TRUE(MatchesGolden("LitSphere_BasicVS_BasicPS", pixels, kLitSize, kLitSize));
    EXPECT_EQ(gl.Errors(), 0);
}

// ----------------------------------------------------------------------------
// FullscreenQuad + PostProcess: the three tonemap operators and FXAA.
// ----------------------------------------------------------------------------
TEST(OpenGLGolden_RHI240_PostProcessACES)
{
    CheckTonemapScene("PostProcess_ACES", Tonemap::ACES, {});
}

TEST(OpenGLGolden_RHI240_PostProcessReinhard)
{
    CheckTonemapScene("PostProcess_Reinhard", Tonemap::Reinhard, {"TONEMAP_REINHARD"});
}

TEST(OpenGLGolden_RHI240_PostProcessUncharted2)
{
    CheckTonemapScene("PostProcess_Uncharted2", Tonemap::Uncharted2, {"TONEMAP_UNCHARTED2"});
}

TEST(OpenGLGolden_RHI240_PostProcessFXAA)
{
    CheckTonemapScene("PostProcess_FXAA", Tonemap::ACES, {"FXAA_PASS"});
}

// ----------------------------------------------------------------------------
// FullscreenQuad + GaussianBlur: horizontal and vertical 9-tap passes.
// ----------------------------------------------------------------------------
TEST(OpenGLGolden_RHI240_GaussianBlurPasses)
{
    GLTestDevice gl;
    RequireLlvmpipe(gl);
    GoldenScene golden{gl};

    const std::vector<uint8_t> lines = MakeBlurInput();
    IRHITexture* input = golden.Texture(kPostSize, kPostSize, PixelFormat::R8G8B8A8_UNORM,
                                        RHITextureUsage::ShaderResource, lines.data());
    // Point sampling keeps every tap on a texel centre, so the CPU weights are exact.
    IRHISampler* pointClamp = golden.Sampler(RHIFilterMode::Nearest, RHIAddressMode::Clamp);
    const float texelSize[4] = {1.0f / kPostSize, 1.0f / kPostSize, 0.0f, 0.0f};
    IRHIBuffer* constants = golden.Constants(texelSize, sizeof(texelSize));

    const auto horizontal =
        golden.FullscreenPass("GaussianBlur.glsl", {"BLUR_HORIZONTAL"}, input, pointClamp, constants);
    const auto vertical = golden.FullscreenPass("GaussianBlur.glsl", {}, input, pointClamp, constants);
    ASSERT_EQ(horizontal.size(), size_t(kPostSize) * kPostSize * 4);
    ASSERT_EQ(vertical.size(), size_t(kPostSize) * kPostSize * 4);

    // Horizontal pass: the white column spreads by the tap weights along the row;
    // the white row is unchanged (the weights sum to 1).
    for (uint32_t tap = 0; tap < 5; ++tap)
    {
        const float w = kBlurWeights[tap];
        EXPECT_TRUE(PixelMatches(horizontal, kPostSize, kBlurLineColumn + tap, 30, {w, w, w}, 1));
        EXPECT_TRUE(PixelMatches(horizontal, kPostSize, kBlurLineColumn - tap, 30, {w, w, w}, 1));
        EXPECT_TRUE(PixelMatches(vertical, kPostSize, 5, kBlurLineRow + tap, {w, w, w}, 1));
        EXPECT_TRUE(PixelMatches(vertical, kPostSize, 5, kBlurLineRow - tap, {w, w, w}, 1));
    }
    EXPECT_TRUE(PixelMatches(horizontal, kPostSize, kBlurLineColumn + 5, 30, {0, 0, 0}, 0));
    EXPECT_TRUE(PixelMatches(horizontal, kPostSize, 5, kBlurLineRow, {1, 1, 1}, 1));
    EXPECT_TRUE(PixelMatches(vertical, kPostSize, 5, kBlurLineRow + 5, {0, 0, 0}, 0));
    EXPECT_TRUE(PixelMatches(vertical, kPostSize, kBlurLineColumn, 30, {1, 1, 1}, 1));

    EXPECT_TRUE(MatchesGolden("GaussianBlur_Horizontal", horizontal, kPostSize, kPostSize));
    EXPECT_TRUE(MatchesGolden("GaussianBlur_Vertical", vertical, kPostSize, kPostSize));
    EXPECT_EQ(gl.Errors(), 0);
}

// ----------------------------------------------------------------------------
// FullscreenQuad + BloomExtract: soft-knee bright pass over the HDR input.
// ----------------------------------------------------------------------------
TEST(OpenGLGolden_RHI240_BloomExtract)
{
    GLTestDevice gl;
    RequireLlvmpipe(gl);
    GoldenScene golden{gl};

    const std::vector<float> hdr = MakeHdrInput();
    IRHITexture* input = golden.Texture(kPostSize, kPostSize, PixelFormat::R32G32B32A32_FLOAT,
                                        RHITextureUsage::ShaderResource, hdr.data());
    IRHISampler* pointClamp = golden.Sampler(RHIFilterMode::Nearest, RHIAddressMode::Clamp);
    IRHIBuffer* constants = golden.Constants(&kBloomConstants, sizeof(kBloomConstants));

    const auto frame = golden.FullscreenPass("BloomExtract.glsl", {}, input, pointClamp, constants);
    ASSERT_EQ(frame.size(), size_t(kPostSize) * kPostSize * 4);
    EXPECT_TRUE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(frame, 0.9));

    for (const auto& probe : kFlatProbes)
        EXPECT_TRUE(PixelMatches(frame, kPostSize, probe[0], probe[1], BloomReference(HdrTile(probe[0], probe[1]))));
    EXPECT_TRUE(PixelMatches(frame, kPostSize, 32, 32, BloomReference(kHdrDisc)));
    // Dark tiles are below the knee and extract to black.
    EXPECT_TRUE(PixelMatches(frame, kPostSize, 8, 8, {0, 0, 0}, 0));

    EXPECT_TRUE(MatchesGolden("BloomExtract", frame, kPostSize, kPostSize));
    EXPECT_EQ(gl.Errors(), 0);
}

#endif // SPARK_OPENGL_SUPPORT
