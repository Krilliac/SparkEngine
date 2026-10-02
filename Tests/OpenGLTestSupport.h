/**
 * @file OpenGLTestSupport.h
 * @brief Shared harness for the real-GL test files (RHI-240): a GLDevice with
 *        KHR_debug errors counted as defects, the SPARK_REQUIRE_OPENGL skip rule,
 *        and small shader/texture helpers.
 *
 * Included only by translation units built with SPARK_OPENGL_SUPPORT. Every
 * test that uses GLTestDevice must end with `EXPECT_EQ(gl.Errors(), 0)`.
 */

#pragma once

#ifdef SPARK_OPENGL_SUPPORT

#include "TestFramework.h"

#include "Graphics/RHI/OpenGL/OpenGLDevice.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace SparkGLTest
{
    /// Driver-reported defects since the last GLTestDevice::Start; reset per test.
    inline int g_glErrorCount = 0;

    inline void APIENTRY CountGLErrors(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei /*length*/,
                                       const GLchar* message, const void* /*userParam*/)
    {
        if (type != GL_DEBUG_TYPE_ERROR && severity != GL_DEBUG_SEVERITY_HIGH)
            return;
        ++g_glErrorCount;
        std::printf("[RHI-240 GL ERROR] test=%s src=0x%x type=0x%x id=%u: %s\n", g_currentTest.c_str(), source, type,
                    id, message);
    }

    /// Drains glGetError so errors are counted even if debug output were dropped. A context
    /// holds at most one flag per error code, so a sane driver drains in a handful of calls;
    /// the bound turns a missing/lost context (glGetError never clearing) into failures
    /// instead of a hang.
    inline int DrainGLErrors()
    {
        int count = 0;
        while (count < 32 && glGetError() != GL_NO_ERROR)
            ++count;
        return count;
    }

    /// A real GLDevice with a debug context and a KHR_debug callback that counts errors.
    struct GLTestDevice
    {
        Spark::RHI::OpenGL::GLDevice device;

        bool Start()
        {
            Spark::RHI::RHIDeviceDesc desc;
            desc.enableDebugLayer = true;
            desc.applicationName = "RHI240";
            if (!device.Initialize(desc))
                return false;
            // Every GL call below needs the device's context to still be current after
            // Initialize (it once was torn down on Windows, making each call a silent no-op).
            if (glGetString(GL_VERSION) == nullptr)
                throw std::runtime_error("GLDevice::Initialize returned true but left no current GL context");

            glEnable(GL_DEBUG_OUTPUT);
            glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
            glDebugMessageCallback(CountGLErrors, nullptr);
            glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
            g_glErrorCount = DrainGLErrors();
            return true;
        }

        int Errors() { return g_glErrorCount + DrainGLErrors(); }

        ~GLTestDevice()
        {
            if (glDebugMessageCallback)
                glDebugMessageCallback(nullptr, nullptr);
        }
    };

    /// Skips (or fails under SPARK_REQUIRE_OPENGL=1) when no GL context exists.
    inline void RequireGL(GLTestDevice& gl)
    {
#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
        // The system libEGL/Mesa cannot be MSan-instrumented, so its writes look uninitialized and
        // MSan aborts inside eglGetDisplay. Real-GL coverage runs in the gcc/clang/ASan lanes.
        // Checked before SPARK_REQUIRE_OPENGL so that setting cannot force the path under MSan.
        SKIP_TEST("MemorySanitizer: system libEGL/Mesa is uninstrumented");
#endif
#endif
        if (gl.Start())
        {
            // These tests exercise the GL 4.5 core backend (#version 450 shaders, DSA-era
            // state). A context below 4.5 -- macOS caps OpenGL at 4.1 -- cannot run them.
            GLint major = 0;
            GLint minor = 0;
            glGetIntegerv(GL_MAJOR_VERSION, &major);
            glGetIntegerv(GL_MINOR_VERSION, &minor);
            if (major < 4 || (major == 4 && minor < 5))
                SKIP_TEST("OpenGL 4.5 core required; context reports " + std::to_string(major) + "." +
                          std::to_string(minor));
            return;
        }
        const char* required = std::getenv("SPARK_REQUIRE_OPENGL");
        if (required && std::string(required) == "1")
            throw std::runtime_error("SPARK_REQUIRE_OPENGL=1 but GLDevice::Initialize failed");
        SKIP_TEST("no OpenGL context available (no EGL/GLX driver: Mesa llvmpipe or a GPU driver)");
    }

    inline std::unique_ptr<Spark::RHI::IRHIShader> MakeShader(Spark::RHI::OpenGL::GLDevice& device,
                                                              Spark::RHI::RHIShaderStage stage,
                                                              const std::string& source,
                                                              std::vector<std::string> defines = {})
    {
        Spark::RHI::RHIShaderDesc desc;
        desc.stage = stage;
        desc.language = Spark::RHI::ShaderLanguage::GLSL;
        desc.sourceCode = source;
        desc.defines = std::move(defines);
        return device.CreateShader(desc);
    }

    inline std::unique_ptr<Spark::RHI::IRHITexture> MakeTexture(
        Spark::RHI::OpenGL::GLDevice& device, uint32_t w, uint32_t h, Spark::RHI::PixelFormat format,
        Spark::RHI::RHITextureUsage usage, Spark::RHI::RHITextureType type = Spark::RHI::RHITextureType::Texture2D,
        uint32_t arraySize = 1)
    {
        Spark::RHI::RHITextureDesc desc;
        desc.width = w;
        desc.height = h;
        desc.format = format;
        desc.usage = usage;
        desc.type = type;
        desc.arraySize = arraySize;
        return device.CreateTexture(desc);
    }

    inline std::string ReadFile(const std::filesystem::path& path)
    {
        std::ifstream file(path);
        std::stringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }

    /// Shipped GLSL sources. Not __FILE__: reproducible optimized builds trim the
    /// source-root prefix (/d1trimfile, -ffile-prefix-map), so __FILE__ is no longer absolute.
    inline std::filesystem::path GLSLDir()
    {
        return std::filesystem::path(SPARK_TEST_SOURCE_DIR) / "Shaders" / "GLSL";
    }
} // namespace SparkGLTest

#endif // SPARK_OPENGL_SUPPORT
