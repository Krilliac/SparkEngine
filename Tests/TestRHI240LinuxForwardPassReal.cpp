/**
 * @file TestRHI240LinuxForwardPassReal.cpp
 * @brief RHI-240 / RHI-230: the Linux forward draw-list pass renders through a bound pipeline.
 *
 * Each test runs the production Linux frame (GraphicsEngine::BeginFrame, RenderScene, EndFrame)
 * on a real SDL2 window, the same way the SDL2 host drives it, with one unit cube submitted
 * through SubmitMeshForRendering. RenderScene drains the draw list through ProcessDrawList, which
 * must bind the basic_vs/basic_ps pipeline and its constant buffers before drawing. The back
 * buffer of the last frame is read back and must show:
 *
 *   - rendered content (not a uniform frame),
 *   - the cube, lit (not black), at the pixel its world position projects to analytically, and the clear colour
 *     at the vertically mirrored pixel, so a Y-flipped image fails,
 *   - the clear colour in every corner,
 *   - at least one draw counted by the backend and none rejected by the pass.
 *
 * Several frames are rendered so state that only the first frame gets right (a depth buffer
 * that is never cleared again, a command buffer that is only begun once) fails too.
 *
 *   - LinuxForwardPass_OpenGLCubeRenders: OpenGL on Mesa llvmpipe (the lane pins
 *     LIBGL_ALWAYS_SOFTWARE/GALLIUM_DRIVER); no GL error may be pending afterwards.
 *   - LinuxForwardPass_VulkanCubeRenders: Vulkan on Lavapipe with VK_LAYER_KHRONOS_validation
 *     forced on through the loader and its log redirected to a file; the layer must report
 *     itself active and log zero validation errors.
 *
 * Software-rasterizer evidence for the forward pass only: the deferred, shadow and post engine
 * passes are not covered, and neither is hardware. Without a display, window or driver the tests
 * skip, or fail under SPARK_REQUIRE_OPENGL=1 / SPARK_REQUIRE_VULKAN_VALIDATION=1 (the dedicated
 * CTest lanes, which run under xvfb-run).
 */

#include "Core/Platform.h"
#include "TestFramework.h"

#if !defined(SPARK_PLATFORM_WINDOWS) && !defined(__APPLE__) && defined(SPARK_SDL2_AVAILABLE) &&                        \
    (defined(SPARK_OPENGL_SUPPORT) || defined(SPARK_VULKAN_SUPPORT))

#include "Graphics/AssetPipeline.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/GraphicsEngineRHI.h"
#include "Graphics/RHI/RHIBridge.h"
#include "Utils/GoldenImageTest.h"

#ifdef SPARK_OPENGL_SUPPORT
#include "Graphics/RHI/OpenGL/OpenGLDevice.h"
#endif
#ifdef SPARK_VULKAN_SUPPORT
#include "Graphics/RHI/Vulkan/VulkanDevice.h"
#endif

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace DirectX;

namespace
{
    // GraphicsEngine::Initialize sizes the Linux swap chain at 1280x720 whatever the window, so
    // the window matches it and the whole back buffer is visible.
    constexpr int kWidth = 1280;
    constexpr int kHeight = 720;
    constexpr int kFrames = 3;
    constexpr float kCubeHeight = 0.8f; ///< World Y of the cube centre: above the view axis
    constexpr float kEyeZ = -4.0f;
    const std::array<float, 4> kClearColour = {0.1f, 0.2f, 0.3f, 1.0f};

    enum class Backend
    {
        OpenGL,
        Vulkan
    };

    /// Sets an environment variable for the scope and restores the previous value.
    class ScopedEnv
    {
      public:
        ScopedEnv(const char* name, const std::string& value) : m_name(name)
        {
            if (const char* previous = std::getenv(name))
                m_previous = previous;
            setenv(name, value.c_str(), 1);
        }
        ~ScopedEnv()
        {
            if (m_previous)
                setenv(m_name.c_str(), m_previous->c_str(), 1);
            else
                unsetenv(m_name.c_str());
        }
        ScopedEnv(const ScopedEnv&) = delete;
        ScopedEnv& operator=(const ScopedEnv&) = delete;

      private:
        std::string m_name;
        std::optional<std::string> m_previous;
    };

    /// The renderer registers its shaders by runtime-relative path ("Shaders/GLSL/...",
    /// "Shaders/SPIRV/..."), so each test runs from the directory that holds them.
    class ScopedWorkingDirectory
    {
      public:
        explicit ScopedWorkingDirectory(const std::filesystem::path& directory)
            : m_previous(std::filesystem::current_path())
        {
            std::filesystem::current_path(directory);
        }
        ~ScopedWorkingDirectory()
        {
            std::error_code error;
            std::filesystem::current_path(m_previous, error);
        }
        ScopedWorkingDirectory(const ScopedWorkingDirectory&) = delete;
        ScopedWorkingDirectory& operator=(const ScopedWorkingDirectory&) = delete;

      private:
        std::filesystem::path m_previous;
    };

    const char* RequireVariable(Backend backend)
    {
        return backend == Backend::OpenGL ? "SPARK_REQUIRE_OPENGL" : "SPARK_REQUIRE_VULKAN_VALIDATION";
    }

    /// A dedicated lane must not pass by skipping, so a missing capability fails there.
    [[noreturn]] void SkipOrFail(Backend backend, const std::string& reason)
    {
        const char* required = std::getenv(RequireVariable(backend));
        if (required != nullptr && std::string(required) == "1")
            throw std::runtime_error(std::string(RequireVariable(backend)) + "=1 but " + reason);
        SKIP_TEST(reason);
    }

    /// The SDL2 window (and, for OpenGL, context) the production host creates for a backend.
    class WindowHost
    {
      public:
        explicit WindowHost(Backend backend)
        {
            if (backend == Backend::OpenGL)
            {
#ifdef SPARK_EGL_SUPPORT
                // As the SDL2 host: EGL builds of GLDevice reuse only a host-owned EGL context.
                SDL_SetHint(SDL_HINT_VIDEO_X11_FORCE_EGL, "1");
#endif
            }
            if (SDL_Init(SDL_INIT_VIDEO) != 0)
                SkipOrFail(backend, std::string("SDL_Init(SDL_INIT_VIDEO) failed: ") + SDL_GetError());
            m_sdlStarted = true;

            Uint32 flags = SDL_WINDOW_HIDDEN;
            if (backend == Backend::OpenGL)
            {
                // GLDevice requires a 4.5 core context (SparkEngineLinuxSDL2.cpp asks for the same).
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
                SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
                SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
                SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
                flags |= SDL_WINDOW_OPENGL;
            }
            else
            {
                flags |= SDL_WINDOW_VULKAN;
            }

            window = SDL_CreateWindow("RHI240 forward pass", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, kWidth,
                                      kHeight, flags);
            if (window == nullptr)
                SkipOrFail(backend, std::string("SDL_CreateWindow failed: ") + SDL_GetError());

            if (backend == Backend::OpenGL)
            {
                m_glContext = SDL_GL_CreateContext(window);
                if (m_glContext == nullptr)
                    SkipOrFail(backend, std::string("SDL_GL_CreateContext failed: ") + SDL_GetError());
                SDL_GL_MakeCurrent(window, m_glContext);
            }
        }

        ~WindowHost()
        {
            if (m_glContext != nullptr)
                SDL_GL_DeleteContext(m_glContext);
            if (window != nullptr)
                SDL_DestroyWindow(window);
            if (m_sdlStarted)
                SDL_Quit();
        }

        WindowHost(const WindowHost&) = delete;
        WindowHost& operator=(const WindowHost&) = delete;

        SDL_Window* window = nullptr;

      private:
        SDL_GLContext m_glContext = nullptr;
        bool m_sdlStarted = false;
    };

    /// Unit cube with per-face normals and UVs, triangulated.
    std::filesystem::path WriteUnitCube()
    {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / "spark_rhi240_forward_cube.obj";
        std::ofstream obj(path, std::ios::trunc);
        obj << "v -0.5 -0.5 -0.5\nv 0.5 -0.5 -0.5\nv 0.5 0.5 -0.5\nv -0.5 0.5 -0.5\n"
               "v -0.5 -0.5 0.5\nv 0.5 -0.5 0.5\nv 0.5 0.5 0.5\nv -0.5 0.5 0.5\n"
               "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
               "vn 0 0 -1\nvn 0 0 1\nvn -1 0 0\nvn 1 0 0\nvn 0 -1 0\nvn 0 1 0\n";
        // Each face: two triangles (a b c) and (a c d) with UV corners 1-2-3 and 1-3-4.
        const int faces[6][5] = {{1, 2, 3, 4, 1}, {6, 5, 8, 7, 2}, {5, 1, 4, 8, 3},
                                 {2, 6, 7, 3, 4}, {5, 6, 2, 1, 5}, {4, 3, 7, 8, 6}};
        for (const auto& face : faces)
        {
            const int n = face[4];
            obj << "f " << face[0] << "/1/" << n << ' ' << face[1] << "/2/" << n << ' ' << face[2] << "/3/" << n
                << '\n';
            obj << "f " << face[0] << "/1/" << n << ' ' << face[2] << "/3/" << n << ' ' << face[3] << "/4/" << n
                << '\n';
        }
        return path;
    }

    XMMATRIX CubeWorld()
    {
        return XMMatrixTranslation(0.0f, kCubeHeight, 0.0f);
    }

    XMMATRIX View()
    {
        return XMMatrixLookAtLH(XMVectorSet(0.0f, 0.0f, kEyeZ, 1.0f), XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f),
                                XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    }

    XMMATRIX Projection()
    {
        return XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), float(kWidth) / float(kHeight), 0.1f, 100.0f);
    }

    /// Pixel (column, row from the top) the cube centre projects to, computed on the CPU.
    std::pair<int, int> ProjectedCubeCentre()
    {
        const XMMATRIX m = XMMatrixMultiply(XMMatrixMultiply(CubeWorld(), View()), Projection());
        // Row vector (0, 0, 0, 1) times m is m's fourth row.
        const float w = m.m[3][3];
        const float ndcX = m.m[3][0] / w;
        const float ndcY = m.m[3][1] / w;
        const int column = static_cast<int>((ndcX + 1.0f) * 0.5f * kWidth);
        const int row = static_cast<int>((1.0f - ndcY) * 0.5f * kHeight);
        return {column, row};
    }

    struct Rgb
    {
        int r = 0;
        int g = 0;
        int b = 0;
    };

    Rgb PixelAt(const std::vector<uint8_t>& rgba, int column, int row)
    {
        const size_t offset = (size_t(row) * kWidth + size_t(column)) * 4;
        return {rgba[offset], rgba[offset + 1], rgba[offset + 2]};
    }

    Rgb ClearRgb()
    {
        auto toByte = [](float channel) { return static_cast<int>(channel * 255.0f + 0.5f); };
        return {toByte(kClearColour[0]), toByte(kClearColour[1]), toByte(kClearColour[2])};
    }

    int MaxChannelDifference(const Rgb& a, const Rgb& b)
    {
        return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
    }

    struct Frame
    {
        std::vector<uint8_t> rgba; ///< Top-down RGBA8 back buffer of the last frame
        uint32_t backendDrawCalls = 0;
        uint32_t engineDrawCalls = 0;
    };

#ifdef SPARK_OPENGL_SUPPORT
    /// GL executes immediately, so the back buffer can be read before EndFrame presents it.
    std::vector<uint8_t> ReadOpenGLBackBuffer()
    {
        glFinish();
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        std::vector<uint8_t> bottomUp(size_t(kWidth) * kHeight * 4);
        glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data());
        std::vector<uint8_t> topDown(bottomUp.size());
        const size_t rowBytes = size_t(kWidth) * 4;
        for (int row = 0; row < kHeight; ++row)
            std::copy_n(bottomUp.data() + size_t(kHeight - 1 - row) * rowBytes, rowBytes,
                        topDown.data() + size_t(row) * rowBytes);
        return topDown;
    }

    int DrainGLErrors()
    {
        int count = 0;
        while (count < 32 && glGetError() != GL_NO_ERROR)
            ++count;
        return count;
    }
#endif

#ifdef SPARK_VULKAN_SUPPORT
    /// Submits what the frame recorded so far, reads the acquired swap-chain image, and reopens
    /// the immediate list so EndFrame closes, submits and presents the frame as usual.
    std::vector<uint8_t> ReadVulkanBackBuffer()
    {
        auto& rhi = Spark::Graphics::Detail::GetRHI();
        auto* device = dynamic_cast<Spark::RHI::Vulkan::VulkanDevice*>(rhi.bridge.GetDevice());
        Spark::RHI::IRHICommandList* cmd = rhi.bridge.GetCommandList();
        Spark::RHI::IRHITexture* backBuffer = rhi.bridge.GetBackBuffer();
        if (device == nullptr || cmd == nullptr || backBuffer == nullptr)
            return {};

        cmd->End();
        device->ExecuteCommandList(cmd);
        device->WaitForIdle();
        std::vector<uint8_t> pixels = device->ReadbackTexture(backBuffer);
        cmd->Begin();

        if (backBuffer->GetFormat() == Spark::RHI::PixelFormat::B8G8R8A8_UNORM)
        {
            for (size_t i = 0; i + 3 < pixels.size(); i += 4)
                std::swap(pixels[i], pixels[i + 2]);
        }
        return pixels;
    }
#endif

    /// Runs kFrames production frames with the cube submitted and reads back the last one.
    Frame RenderCubeFrames(GraphicsEngine& engine, Backend backend)
    {
        const std::string meshPath = WriteUnitCube().string();
        AssetPipeline* assets = engine.GetAssetPipeline();
        if (assets == nullptr || !assets->LoadAsset(meshPath, AssetType::Mesh))
            throw std::runtime_error("the unit cube did not load through the AssetPipeline");

        GraphicsSettings settings = engine.GetGraphicsSettings();
        for (size_t i = 0; i < kClearColour.size(); ++i)
            settings.clearColor[i] = kClearColour[i];
        engine.SetGraphicsSettings(settings);

        Frame frame;
        for (int index = 0; index < kFrames; ++index)
        {
            engine.SubmitMeshForRendering(meshPath, "", CubeWorld(), /*castShadows*/ true);
            engine.BeginFrame();
            engine.RenderScene(View(), Projection(), {});
            if (index == kFrames - 1)
            {
                frame.backendDrawCalls = Spark::Graphics::Detail::GetRHI().bridge.GetFrameStatistics().drawCalls;
#ifdef SPARK_OPENGL_SUPPORT
                if (backend == Backend::OpenGL)
                    frame.rgba = ReadOpenGLBackBuffer();
#endif
#ifdef SPARK_VULKAN_SUPPORT
                if (backend == Backend::Vulkan)
                    frame.rgba = ReadVulkanBackBuffer();
#endif
            }
            engine.EndFrame();
        }
        frame.engineDrawCalls = engine.GetStatistics().drawCalls;
        return frame;
    }

    void ExpectCubeFrame(const Frame& frame)
    {
        ASSERT_EQ(frame.rgba.size(), size_t(kWidth) * kHeight * 4);
        EXPECT_TRUE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(frame.rgba, 0.995));

        const Rgb clear = ClearRgb();
        for (const auto& [column, row] :
             {std::pair{2, 2}, std::pair{kWidth - 3, 2}, std::pair{2, kHeight - 3}, std::pair{kWidth - 3, kHeight - 3}})
            EXPECT_TRUE(MaxChannelDifference(PixelAt(frame.rgba, column, row), clear) <= 2);

        const auto [column, row] = ProjectedCubeCentre();
        const Rgb cube = PixelAt(frame.rgba, column, row);
        const Rgb mirrored = PixelAt(frame.rgba, column, kHeight - 1 - row);
        std::printf("[RHI-240 FORWARD] cube centre (%d,%d) rgb(%d,%d,%d); mirrored row %d rgb(%d,%d,%d); "
                    "backend draws %u, engine draws %u\n",
                    column, row, cube.r, cube.g, cube.b, kHeight - 1 - row, mirrored.r, mirrored.g, mirrored.b,
                    frame.backendDrawCalls, frame.engineDrawCalls);
        EXPECT_GT(MaxChannelDifference(cube, clear), 24);
        // The white default albedo under the default sun and ambient shades the lit front face a
        // light grey; black would mean the draw ran without its texture or constants.
        EXPECT_GT(std::min({cube.r, cube.g, cube.b}), 96);
        EXPECT_TRUE(MaxChannelDifference(mirrored, clear) <= 2);

        EXPECT_GE(frame.backendDrawCalls, 1u);
        EXPECT_GE(frame.engineDrawCalls, 1u);
        EXPECT_EQ(Spark::Graphics::Detail::GetRHI().basicForward.rejectedDraws, 0u);
    }

    void RequireActiveBackend(Backend backend)
    {
        const auto expected =
            backend == Backend::OpenGL ? Spark::RHI::GraphicsBackend::OpenGL : Spark::RHI::GraphicsBackend::Vulkan;
        const auto active = Spark::Graphics::Detail::GetRHI().bridge.GetActiveBackend();
        if (active != expected)
            SkipOrFail(backend,
                       "the RHI bridge came up on " + Spark::Graphics::Detail::GetRHI().bridge.GetBackendName());
    }
} // namespace

#ifdef SPARK_OPENGL_SUPPORT
TEST(LinuxForwardPass_OpenGLCubeRenders)
{
    // Shaders/GLSL is read from the source tree: the runtime directory stages only HLSL and SPIR-V.
    ScopedWorkingDirectory cwd(SPARK_TEST_SOURCE_DIR);
    ScopedEnv backendRequest("SPARK_RHI_BACKEND", "opengl");
    WindowHost host(Backend::OpenGL);

    GraphicsEngine engine;
    if (FAILED(engine.Initialize(host.window)))
        SkipOrFail(Backend::OpenGL, "GraphicsEngine::Initialize failed on the OpenGL window");
    RequireActiveBackend(Backend::OpenGL);
    std::printf("[RHI-240 FORWARD] OpenGL GL_RENDERER=\"%s\"\n",
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    const Frame frame = RenderCubeFrames(engine, Backend::OpenGL);
    ExpectCubeFrame(frame);
    EXPECT_EQ(DrainGLErrors(), 0);
    engine.Shutdown();
}
#endif // SPARK_OPENGL_SUPPORT

#if defined(SPARK_VULKAN_SUPPORT) && defined(SPARK_TEST_SPIRV_DIR)
TEST(LinuxForwardPass_VulkanCubeRenders)
{
    // The runtime directory the build stages Shaders/SPIRV into.
    ScopedWorkingDirectory cwd(std::filesystem::path(SPARK_TEST_SPIRV_DIR).parent_path().parent_path());
    ScopedEnv backendRequest("SPARK_RHI_BACKEND", "vulkan");

    // Force the Khronos validation layer on through the loader, whatever the build type, and
    // send its report to a file. The info report proves the layer ran; errors are counted.
    const std::filesystem::path validationLog =
        std::filesystem::temp_directory_path() / "spark_rhi240_forward_vulkan_validation.log";
    std::filesystem::remove(validationLog);
    ScopedEnv layers("VK_INSTANCE_LAYERS", "VK_LAYER_KHRONOS_validation");
    ScopedEnv action("VK_KHRONOS_VALIDATION_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_LOG_MSG");
    ScopedEnv reportFlags("VK_KHRONOS_VALIDATION_REPORT_FLAGS", "error,info");
    ScopedEnv logFile("VK_KHRONOS_VALIDATION_LOG_FILENAME", validationLog.string());

    {
        WindowHost host(Backend::Vulkan);
        GraphicsEngine engine;
        if (FAILED(engine.Initialize(host.window)))
            SkipOrFail(Backend::Vulkan, "GraphicsEngine::Initialize failed on the Vulkan window");
        RequireActiveBackend(Backend::Vulkan);
        if (!Spark::Graphics::Detail::GetRHI().bridge.GetCapabilities().isSoftwareDevice)
            SkipOrFail(Backend::Vulkan, "the Vulkan device is not Lavapipe");

        const Frame frame = RenderCubeFrames(engine, Backend::Vulkan);
        ExpectCubeFrame(frame);
        engine.Shutdown();
    }

    // The layer flushes and closes its log when the instance is destroyed (engine shutdown).
    std::ifstream logStream(validationLog);
    std::stringstream log;
    log << logStream.rdbuf();
    const std::string report = log.str();
    size_t errors = 0;
    for (size_t at = report.find("Validation Error"); at != std::string::npos;
         at = report.find("Validation Error", at + 1))
        ++errors;
    if (report.find("Validation Layer Active") == std::string::npos)
        SkipOrFail(Backend::Vulkan,
                   "VK_LAYER_KHRONOS_validation did not report itself active in " + validationLog.string());
    if (errors != 0)
        std::printf("[RHI-240 FORWARD] Vulkan validation report:\n%s\n", report.c_str());
    EXPECT_EQ(errors, size_t(0));
}
#endif // SPARK_VULKAN_SUPPORT && SPARK_TEST_SPIRV_DIR

#endif // !Windows && !Apple && SDL2 && (OpenGL || Vulkan)
