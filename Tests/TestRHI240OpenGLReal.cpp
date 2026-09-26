/**
 * @file TestRHI240OpenGLReal.cpp
 * @brief RHI-240: real OpenGL backend runs with KHR_debug output counted as defects
 *
 * Every test creates a real GLDevice (Mesa llvmpipe on the Linux CI/cloud hosts),
 * installs a KHR_debug callback that counts GL_DEBUG_TYPE_ERROR / HIGH severity
 * messages, drives one backend path, and requires zero GL errors plus a
 * functional read-back where the path produces pixels or bytes.
 *
 * llvmpipe results are software-rasterizer evidence only; they are not driver
 * certification for any hardware GL implementation.
 *
 * When no GL context can be created the tests SKIP, unless SPARK_REQUIRE_OPENGL=1
 * (set by the dedicated "opengl" CTest lane) turns that into a failure so the
 * lane can never pass by running nothing.
 */

#include "TestFramework.h"

#ifdef SPARK_OPENGL_SUPPORT

#include "Graphics/RHI/OpenGL/OpenGLDevice.h"
#include "Graphics/RHI/RHIFactory.h"

#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Spark::RHI;
using Spark::RHI::OpenGL::GLDevice;

namespace
{
    // Counts driver-reported defects; reset per test by GLTestDevice.
    int g_glErrorCount = 0;

    void APIENTRY CountGLErrors(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei /*length*/,
                                const GLchar* message, const void* /*userParam*/)
    {
        if (type != GL_DEBUG_TYPE_ERROR && severity != GL_DEBUG_SEVERITY_HIGH)
            return;
        ++g_glErrorCount;
        std::printf("[RHI-240 GL ERROR] test=%s src=0x%x type=0x%x id=%u: %s\n", g_currentTest.c_str(), source, type,
                    id, message);
    }

    // Drains glGetError so errors are counted even if debug output were dropped. A context
    // holds at most one flag per error code, so a sane driver drains in a handful of calls;
    // the bound turns a missing/lost context (glGetError never clearing) into failures
    // instead of a hang.
    int DrainGLErrors()
    {
        int count = 0;
        while (count < 32 && glGetError() != GL_NO_ERROR)
            ++count;
        return count;
    }

    struct GLTestDevice
    {
        GLDevice device;

        bool Start()
        {
            RHIDeviceDesc desc;
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

    // Skips (or fails under SPARK_REQUIRE_OPENGL=1) when no GL context exists.
    void RequireGL(GLTestDevice& gl)
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
            return;
        const char* required = std::getenv("SPARK_REQUIRE_OPENGL");
        if (required && std::string(required) == "1")
            throw std::runtime_error("SPARK_REQUIRE_OPENGL=1 but GLDevice::Initialize failed");
        SKIP_TEST("no OpenGL context available (EGL/llvmpipe missing)");
    }

    std::unique_ptr<IRHIShader> MakeShader(GLDevice& device, RHIShaderStage stage, const std::string& source,
                                           std::vector<std::string> defines = {})
    {
        RHIShaderDesc desc;
        desc.stage = stage;
        desc.language = ShaderLanguage::GLSL;
        desc.sourceCode = source;
        desc.defines = std::move(defines);
        return device.CreateShader(desc);
    }

    std::unique_ptr<IRHITexture> MakeTexture(GLDevice& device, uint32_t w, uint32_t h, PixelFormat format,
                                             RHITextureUsage usage, RHITextureType type = RHITextureType::Texture2D,
                                             uint32_t arraySize = 1)
    {
        RHITextureDesc desc;
        desc.width = w;
        desc.height = h;
        desc.format = format;
        desc.usage = usage;
        desc.type = type;
        desc.arraySize = arraySize;
        return device.CreateTexture(desc);
    }

    std::array<uint8_t, 4> ReadPixelRGBA8(IRHITexture* texture, uint32_t x, uint32_t y)
    {
        const GLuint tex = static_cast<GLuint>(reinterpret_cast<uintptr_t>(texture->GetNativeHandle()));
        std::array<uint8_t, 4> px{};
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTextureSubImage(tex, 0, static_cast<GLint>(x), static_cast<GLint>(y), 0, 1, 1, 1, GL_RGBA,
                             GL_UNSIGNED_BYTE, 4, px.data());
        return px;
    }

    std::string ReadFile(const std::filesystem::path& path)
    {
        std::ifstream file(path);
        std::stringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }

    std::filesystem::path GLSLDir()
    {
        // Not __FILE__: reproducible optimized builds trim the source-root prefix
        // (/d1trimfile, -ffile-prefix-map), so __FILE__ is no longer absolute.
        return std::filesystem::path(SPARK_TEST_SOURCE_DIR) / "Shaders" / "GLSL";
    }

    const char* kColorVS = R"(#version 450 core
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 vColor;
void main() { vColor = inColor; gl_Position = vec4(inPos, 0.0, 1.0); }
)";

    const char* kColorPS = R"(#version 450 core
layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 outColor;
void main() { outColor = vColor; }
)";

    struct ColorVertex
    {
        float x, y;
        uint8_t rgba[4];
    };

    RHIPipelineStateDesc ColorPipelineDesc()
    {
        RHIPipelineStateDesc desc;
        desc.inputLayout.elements.push_back({"POSITION", 0, RHIVertexFormat::Float2, 0, 0, false, 0});
        desc.inputLayout.elements.push_back({"COLOR", 0, RHIVertexFormat::UNorm8x4, 0, 8, false, 0});
        desc.rasterizer.cullMode = RHICullMode::None;
        desc.depthStencil.depthEnable = false;
        desc.depthStencil.depthWrite = false;
        return desc;
    }
} // namespace

// ----------------------------------------------------------------------------
// Harness: KHR_debug output is live and the device requests a debug context
// ----------------------------------------------------------------------------
TEST(OpenGL_RHI240_DebugOutputCountsErrors)
{
    GLTestDevice gl;
    RequireGL(gl);

    EXPECT_EQ(gl.Errors(), 0);

    GLint flags = 0;
    glGetIntegerv(GL_CONTEXT_FLAGS, &flags);
    EXPECT_TRUE((flags & GL_CONTEXT_FLAG_DEBUG_BIT) != 0);

    // A deliberate invalid enum proves the counter actually observes driver errors.
    glEnable(0xDEAD);
    EXPECT_GE(gl.Errors(), 1);
    g_glErrorCount = 0;
}

// ----------------------------------------------------------------------------
// Device row: the lane label (software vs hardware) must match the context the
// tests actually ran on, so a hardware run cannot be reported under the
// llvmpipe label and vice versa. SPARK_GL_EXPECT_ROW is set by the CTest lane.
//
// The row is classified here from the raw GL_RENDERER string with the test's own
// list of known CPU rasterizers, not from GLDevice's isSoftwareDevice flag, so a
// product classifier that misses a software renderer fails this test instead of
// silently labelling it a hardware row.
// ----------------------------------------------------------------------------
namespace
{
    bool IsKnownSoftwareGLRenderer(std::string renderer)
    {
        for (char& c : renderer)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        // Mesa llvmpipe, Mesa softpipe, Mesa swrast/"Software Rasterizer", Windows
        // OpenGL 1.1 fallback, WARP-backed GL on Windows, Apple's CPU fallback.
        const std::array<const char*, 7> known = {"llvmpipe",
                                                  "softpipe",
                                                  "swrast",
                                                  "software rasterizer",
                                                  "gdi generic",
                                                  "microsoft basic render driver",
                                                  "apple software renderer"};
        for (const char* name : known)
        {
            if (renderer.find(name) != std::string::npos)
                return true;
        }
        return false;
    }
} // namespace

TEST(OpenGL_RHI240_SoftwareRendererClassifierCases)
{
    EXPECT_TRUE(IsKnownSoftwareGLRenderer("llvmpipe (LLVM 17.0.6, 256 bits)"));
    EXPECT_TRUE(IsKnownSoftwareGLRenderer("softpipe"));
    EXPECT_TRUE(IsKnownSoftwareGLRenderer("Software Rasterizer"));
    EXPECT_TRUE(IsKnownSoftwareGLRenderer("GDI Generic"));
    EXPECT_TRUE(IsKnownSoftwareGLRenderer("D3D12 (Microsoft Basic Render Driver)"));
    EXPECT_TRUE(IsKnownSoftwareGLRenderer("Apple Software Renderer"));
    EXPECT_FALSE(IsKnownSoftwareGLRenderer("NVIDIA GeForce RTX 4070/PCIe/SSE2"));
    EXPECT_FALSE(IsKnownSoftwareGLRenderer("AMD Radeon RX 7800 XT (radeonsi, navi32, LLVM 17.0.6, DRM 3.54)"));
    EXPECT_FALSE(IsKnownSoftwareGLRenderer("Mesa Intel(R) UHD Graphics 630 (CFL GT2)"));
}

TEST(OpenGL_RHI240_DeviceRowMatchesLane)
{
    GLTestDevice gl;
    RequireGL(gl);

    const RHIDeviceCapabilities& caps = gl.device.GetCapabilities();
    const char* liveRenderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* liveVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    ASSERT_TRUE(liveRenderer != nullptr);
    ASSERT_TRUE(liveVersion != nullptr);

    const bool softwareRenderer = IsKnownSoftwareGLRenderer(liveRenderer);
    const std::string row = softwareRenderer ? "software" : "hardware";
    std::printf("[RHI-240 GL DEVICE] GL_RENDERER=\"%s\" GL_VERSION=\"%s\" isSoftwareDevice=%s row=%s\n", liveRenderer,
                liveVersion, caps.isSoftwareDevice ? "true" : "false", row.c_str());

    // Smoke check only: both sides read glGetString on the same context.
    EXPECT_EQ(caps.deviceName, std::string(liveRenderer));
    EXPECT_EQ(caps.apiVersion, std::string(liveVersion));

    // The product's software/hardware flag must agree with the independent classification.
    if (caps.isSoftwareDevice != softwareRenderer)
        throw std::runtime_error("GLDevice isSoftwareDevice=" + std::string(caps.isSoftwareDevice ? "true" : "false") +
                                 " disagrees with GL_RENDERER=\"" + std::string(liveRenderer) + "\" (" + row + ")");

    const char* expected = std::getenv("SPARK_GL_EXPECT_ROW");
    if (expected != nullptr && expected[0] != '\0')
    {
        const std::string expectedRow(expected);
        if (expectedRow != "software" && expectedRow != "hardware")
            throw std::runtime_error("SPARK_GL_EXPECT_ROW must be 'software' or 'hardware', got '" + expectedRow + "'");
        if (expectedRow != row)
            throw std::runtime_error("SPARK_GL_EXPECT_ROW=" + expectedRow + " but the GL context is a " + row +
                                     " device (GL_RENDERER=\"" + std::string(liveRenderer) + "\")");
    }
    EXPECT_EQ(gl.Errors(), 0);
}

// ----------------------------------------------------------------------------
// Buffers: Dynamic/Staging storage must accept UpdateBuffer; ReadBack must map
// ----------------------------------------------------------------------------
TEST(OpenGL_RHI240_BufferUpdateAndMapMatchAccess)
{
    GLTestDevice gl;
    RequireGL(gl);

    std::array<uint8_t, 64> pattern{};
    for (size_t i = 0; i < pattern.size(); ++i)
        pattern[i] = static_cast<uint8_t>(i * 3 + 1);

    for (RHIBufferAccess access : {RHIBufferAccess::Static, RHIBufferAccess::Dynamic, RHIBufferAccess::Staging})
    {
        RHIBufferDesc desc;
        desc.size = pattern.size();
        desc.usage = RHIBufferUsage::Constant;
        desc.access = access;
        auto buffer = gl.device.CreateBuffer(desc);
        ASSERT_TRUE(buffer != nullptr);

        gl.device.UpdateBuffer(buffer.get(), pattern.data(), pattern.size(), 0);

        std::array<uint8_t, 64> readBack{};
        const GLuint name = static_cast<GLuint>(reinterpret_cast<uintptr_t>(buffer->GetNativeHandle()));
        glGetNamedBufferSubData(name, 0, static_cast<GLsizeiptr>(readBack.size()), readBack.data());
        EXPECT_TRUE(readBack == pattern);
    }

    RHIBufferDesc readBackDesc;
    readBackDesc.size = pattern.size();
    readBackDesc.access = RHIBufferAccess::ReadBack;
    readBackDesc.initialData = pattern.data();
    auto readBackBuffer = gl.device.CreateBuffer(readBackDesc);
    ASSERT_TRUE(readBackBuffer != nullptr);
    void* mapped = gl.device.MapBuffer(readBackBuffer.get());
    EXPECT_TRUE(mapped != nullptr);
    if (mapped)
    {
        EXPECT_EQ(std::memcmp(mapped, pattern.data(), pattern.size()), 0);
        gl.device.UnmapBuffer(readBackBuffer.get());
    }

    EXPECT_EQ(gl.Errors(), 0);
}

// ----------------------------------------------------------------------------
// Vertex input: SetVertexBuffer must feed the pipeline VAO (stride from buffer),
// and an index buffer bound before the pipeline must survive the VAO switch.
// ----------------------------------------------------------------------------
TEST(OpenGL_RHI240_VertexAndIndexBuffersReachVAO)
{
    GLTestDevice gl;
    RequireGL(gl);

    auto vs = MakeShader(gl.device, RHIShaderStage::Vertex, kColorVS);
    auto ps = MakeShader(gl.device, RHIShaderStage::Pixel, kColorPS);
    ASSERT_TRUE(vs && ps);
    auto pso = gl.device.CreatePipelineState(ColorPipelineDesc(), vs.get(), ps.get());
    ASSERT_TRUE(pso != nullptr);

    const ColorVertex vertices[3] = {
        {-1.0f, -1.0f, {255, 0, 0, 255}}, {3.0f, -1.0f, {255, 0, 0, 255}}, {-1.0f, 3.0f, {255, 0, 0, 255}}};
    RHIBufferDesc vbDesc;
    vbDesc.size = sizeof(vertices);
    vbDesc.stride = sizeof(ColorVertex);
    vbDesc.usage = RHIBufferUsage::Vertex;
    vbDesc.initialData = vertices;
    auto vb = gl.device.CreateBuffer(vbDesc);

    const uint32_t indices[3] = {0, 1, 2};
    RHIBufferDesc ibDesc;
    ibDesc.size = sizeof(indices);
    ibDesc.stride = sizeof(uint32_t);
    ibDesc.usage = RHIBufferUsage::Index;
    ibDesc.initialData = indices;
    auto ib = gl.device.CreateBuffer(ibDesc);

    auto target = MakeTexture(gl.device, 16, 16, PixelFormat::R8G8B8A8_UNORM, RHITextureUsage::RenderTarget);
    ASSERT_TRUE(vb && ib && target);

    IRHICommandList* cmd = gl.device.GetImmediateCommandList();
    IRHITexture* rts[] = {target.get()};
    cmd->SetRenderTargets(rts, 1, nullptr);
    const float black[4] = {0, 0, 0, 1};
    cmd->ClearRenderTarget(target.get(), black);
    cmd->SetViewport({0, 0, 16, 16, 0, 1});
    cmd->SetIndexBuffer(ib.get(), 0); // before the pipeline: must land on the pipeline VAO
    cmd->SetPipelineState(pso.get());
    cmd->SetVertexBuffer(vb.get(), 0, 0);
    cmd->DrawIndexed(3, 0, 0);
    gl.device.WaitForIdle();

    const auto px = ReadPixelRGBA8(target.get(), 8, 8);
    EXPECT_EQ(px[0], 255);
    EXPECT_EQ(px[1], 0);
    EXPECT_EQ(px[2], 0);
    EXPECT_EQ(gl.Errors(), 0);
}

// ----------------------------------------------------------------------------
// Dynamic buffers: TransientBufferAllocator keeps them mapped across the frame's
// draws, so the mapping must be persistent or every such draw is a GL error.
// ----------------------------------------------------------------------------
TEST(OpenGL_RHI240_DrawFromMappedDynamicBuffer)
{
    GLTestDevice gl;
    RequireGL(gl);

    auto vs = MakeShader(gl.device, RHIShaderStage::Vertex, kColorVS);
    auto ps = MakeShader(gl.device, RHIShaderStage::Pixel, kColorPS);
    ASSERT_TRUE(vs && ps);
    auto pso = gl.device.CreatePipelineState(ColorPipelineDesc(), vs.get(), ps.get());
    auto target = MakeTexture(gl.device, 8, 8, PixelFormat::R8G8B8A8_UNORM, RHITextureUsage::RenderTarget);

    RHIBufferDesc vbDesc;
    vbDesc.size = 3 * sizeof(ColorVertex);
    vbDesc.stride = sizeof(ColorVertex);
    vbDesc.usage = RHIBufferUsage::Vertex;
    vbDesc.access = RHIBufferAccess::Dynamic;
    auto vb = gl.device.CreateBuffer(vbDesc);
    ASSERT_TRUE(pso && target && vb);

    auto* mapped = static_cast<ColorVertex*>(gl.device.MapBuffer(vb.get()));
    ASSERT_TRUE(mapped != nullptr);
    mapped[0] = {-1.0f, -1.0f, {0, 255, 0, 255}};
    mapped[1] = {3.0f, -1.0f, {0, 255, 0, 255}};
    mapped[2] = {-1.0f, 3.0f, {0, 255, 0, 255}};

    IRHICommandList* cmd = gl.device.GetImmediateCommandList();
    IRHITexture* rts[] = {target.get()};
    cmd->SetRenderTargets(rts, 1, nullptr);
    cmd->SetViewport({0, 0, 8, 8, 0, 1});
    cmd->SetPipelineState(pso.get());
    cmd->SetVertexBuffer(vb.get(), 0, 0);
    cmd->Draw(3, 0); // buffer still mapped, as in the transient allocator's frame
    gl.device.UnmapBuffer(vb.get());
    gl.device.WaitForIdle();

    const auto px = ReadPixelRGBA8(target.get(), 4, 4);
    EXPECT_EQ(px[1], 255);
    EXPECT_EQ(gl.Errors(), 0);
}

// ----------------------------------------------------------------------------
// Input layout translation: integer formats use integer attribs, UNorm8x4 is
// normalized, per-instance elements get a binding divisor.
// ----------------------------------------------------------------------------
TEST(OpenGL_RHI240_InputLayoutFormatsTranslate)
{
    GLTestDevice gl;
    RequireGL(gl);

    const char* vsSource = R"(#version 450 core
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in ivec2 inIds;
layout(location = 3) in vec4 inInstanceOffset;
layout(location = 0) out vec4 vColor;
void main() { vColor = inColor + vec4(inIds, 0, 0) + inInstanceOffset; gl_Position = vec4(inPos, 0.0, 1.0); }
)";
    auto vs = MakeShader(gl.device, RHIShaderStage::Vertex, vsSource);
    auto ps = MakeShader(gl.device, RHIShaderStage::Pixel, kColorPS);
    ASSERT_TRUE(vs && ps);

    RHIPipelineStateDesc desc = ColorPipelineDesc();
    desc.inputLayout.elements.push_back({"IDS", 0, RHIVertexFormat::Int2, 0, 12, false, 0});
    desc.inputLayout.elements.push_back({"INSTANCE", 0, RHIVertexFormat::Float4, 1, 0, true, 1});
    auto pso = gl.device.CreatePipelineState(desc, vs.get(), ps.get());
    ASSERT_TRUE(pso != nullptr);

    const GLuint vao = static_cast<OpenGL::GLPipelineState*>(pso.get())->GetGLVAO();
    GLint isInteger = 0;
    GLint normalized = 0;
    GLint divisor = 0;
    glGetVertexArrayIndexediv(vao, 2, GL_VERTEX_ATTRIB_ARRAY_INTEGER, &isInteger);
    glGetVertexArrayIndexediv(vao, 1, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &normalized);
    glGetVertexArrayIndexediv(vao, 3, GL_VERTEX_ATTRIB_ARRAY_DIVISOR, &divisor);
    EXPECT_EQ(isInteger, GL_TRUE);
    EXPECT_EQ(normalized, GL_TRUE);
    EXPECT_EQ(divisor, 1);
    EXPECT_EQ(gl.Errors(), 0);
}

// ----------------------------------------------------------------------------
// Clears: D3D semantics — clear the named target, unaffected by pipeline
// color/depth write masks or scissor, and leave the pipeline state intact.
// ----------------------------------------------------------------------------
TEST(OpenGL_RHI240_ClearsHonorTargetAndIgnoreMasks)
{
    GLTestDevice gl;
    RequireGL(gl);

    auto vs = MakeShader(gl.device, RHIShaderStage::Vertex, kColorVS);
    auto ps = MakeShader(gl.device, RHIShaderStage::Pixel, kColorPS);
    ASSERT_TRUE(vs && ps);
    RHIPipelineStateDesc desc = ColorPipelineDesc();
    desc.blend.renderTargets[0].writeMask = 0;
    desc.depthStencil.depthWrite = false;
    desc.rasterizer.scissorEnable = true;
    auto pso = gl.device.CreatePipelineState(desc, vs.get(), ps.get());
    ASSERT_TRUE(pso != nullptr);

    auto boundTarget = MakeTexture(gl.device, 8, 8, PixelFormat::R8G8B8A8_UNORM, RHITextureUsage::RenderTarget);
    auto otherTarget = MakeTexture(gl.device, 8, 8, PixelFormat::R8G8B8A8_UNORM, RHITextureUsage::RenderTarget);
    auto depth = MakeTexture(gl.device, 8, 8, PixelFormat::D32_FLOAT, RHITextureUsage::DepthStencil);
    ASSERT_TRUE(boundTarget && otherTarget && depth);

    IRHICommandList* cmd = gl.device.GetImmediateCommandList();
    IRHITexture* rts[] = {boundTarget.get()};
    cmd->SetRenderTargets(rts, 1, depth.get());
    cmd->SetPipelineState(pso.get());
    cmd->SetScissorRect({0, 0, 0, 0});

    const float blue[4] = {0, 0, 1, 1};
    const float green[4] = {0, 1, 0, 1};
    cmd->ClearRenderTarget(boundTarget.get(), blue);
    cmd->ClearRenderTarget(otherTarget.get(), green); // not bound: must still be cleared
    cmd->ClearDepthStencil(depth.get(), 0.25f, 0);
    gl.device.WaitForIdle();

    const auto bound = ReadPixelRGBA8(boundTarget.get(), 4, 4);
    const auto other = ReadPixelRGBA8(otherTarget.get(), 4, 4);
    EXPECT_EQ(bound[2], 255);
    EXPECT_EQ(bound[1], 0);
    EXPECT_EQ(other[1], 255);
    EXPECT_EQ(other[2], 0);

    float depthValue = -1.0f;
    const GLuint depthTex = static_cast<GLuint>(reinterpret_cast<uintptr_t>(depth->GetNativeHandle()));
    glGetTextureSubImage(depthTex, 0, 4, 4, 0, 1, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, sizeof(float), &depthValue);
    EXPECT_NEAR(depthValue, 0.25f, 1e-4f);

    // Pipeline masks must be restored after the clears
    GLboolean depthMask = GL_TRUE;
    GLboolean colorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
    EXPECT_EQ(depthMask, GL_FALSE);
    EXPECT_EQ(colorMask[0], GL_FALSE);
    EXPECT_TRUE(glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE);
    EXPECT_EQ(gl.Errors(), 0);
}

// ----------------------------------------------------------------------------
// Texture uploads: client format/type must match the storage format, including
// unaligned rows, SNORM, packed float, compressed, array slices, and cube faces.
// ----------------------------------------------------------------------------
TEST(OpenGL_RHI240_UpdateTextureMatchesFormats)
{
    GLTestDevice gl;
    RequireGL(gl);
    auto name = [](const std::unique_ptr<IRHITexture>& t)
    { return static_cast<GLuint>(reinterpret_cast<uintptr_t>(t->GetNativeHandle())); };
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    // R8 with a 3-texel row: default GL_UNPACK_ALIGNMENT=4 would skew rows
    {
        auto tex = MakeTexture(gl.device, 3, 2, PixelFormat::R8_UNORM, RHITextureUsage::ShaderResource);
        const uint8_t data[6] = {10, 20, 30, 40, 50, 60};
        gl.device.UpdateTexture(tex.get(), data, 0, 0);
        uint8_t out[6] = {};
        glGetTextureImage(name(tex), 0, GL_RED, GL_UNSIGNED_BYTE, sizeof(out), out);
        EXPECT_EQ(std::memcmp(out, data, sizeof(data)), 0);
    }

    // SNORM: client type must be signed bytes
    {
        auto tex = MakeTexture(gl.device, 1, 1, PixelFormat::R8G8B8A8_SNORM, RHITextureUsage::ShaderResource);
        const int8_t data[4] = {-127, 127, 0, 64};
        gl.device.UpdateTexture(tex.get(), data, 0, 0);
        int8_t out[4] = {};
        glGetTextureImage(name(tex), 0, GL_RGBA, GL_BYTE, sizeof(out), out);
        EXPECT_EQ(std::memcmp(out, data, sizeof(data)), 0);
    }

    // R11G11B10_FLOAT: packed 32-bit texel must be uploaded as packed float
    {
        auto tex = MakeTexture(gl.device, 1, 1, PixelFormat::R11G11B10_FLOAT, RHITextureUsage::ShaderResource);
        // 1.0 in 11-bit float = 0x3C0 (exp 15, mantissa 0); 1.0 in 10-bit = 0x1E0
        const uint32_t packed = 0x3C0u | (0x3C0u << 11) | (0x1E0u << 22);
        gl.device.UpdateTexture(tex.get(), &packed, 0, 0);
        float out[3] = {};
        glGetTextureImage(name(tex), 0, GL_RGB, GL_FLOAT, sizeof(out), out);
        EXPECT_NEAR(out[0], 1.0f, 1e-3f);
        EXPECT_NEAR(out[1], 1.0f, 1e-3f);
        EXPECT_NEAR(out[2], 1.0f, 1e-3f);
    }

    // BC1 (compressed): must use the compressed upload entry point
    if (GLAD_GL_EXT_texture_compression_s3tc)
    {
        auto tex = MakeTexture(gl.device, 4, 4, PixelFormat::BC1_UNORM, RHITextureUsage::ShaderResource);
        const uint8_t block[8] = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; // all texels white
        gl.device.UpdateTexture(tex.get(), block, 0, 0);
        uint8_t out[16 * 4] = {};
        glGetTextureImage(name(tex), 0, GL_RGBA, GL_UNSIGNED_BYTE, sizeof(out), out);
        EXPECT_EQ(out[0], 255);
    }

    // Texture2DArray: arraySlice selects the layer
    {
        auto tex = MakeTexture(gl.device, 2, 2, PixelFormat::R8G8B8A8_UNORM, RHITextureUsage::ShaderResource,
                               RHITextureType::Texture2DArray, 2);
        std::vector<uint8_t> data(2 * 2 * 4, 200);
        gl.device.UpdateTexture(tex.get(), data.data(), 0, 1);
        uint8_t slice1[16] = {};
        glGetTextureSubImage(name(tex), 0, 0, 0, 1, 2, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, sizeof(slice1), slice1);
        EXPECT_EQ(slice1[0], 200);
    }

    // TextureCube: arraySlice selects the face
    {
        auto tex = MakeTexture(gl.device, 2, 2, PixelFormat::R8G8B8A8_UNORM, RHITextureUsage::ShaderResource,
                               RHITextureType::TextureCube);
        std::vector<uint8_t> data(2 * 2 * 4, 77);
        gl.device.UpdateTexture(tex.get(), data.data(), 0, 3);
        uint8_t face3[16] = {};
        glGetTextureSubImage(name(tex), 0, 0, 0, 3, 2, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, sizeof(face3), face3);
        EXPECT_EQ(face3[0], 77);
    }

    EXPECT_EQ(gl.Errors(), 0);
}

// ----------------------------------------------------------------------------
// Depth-stencil: stencil func/ops/read mask from the pipeline reach GL state
// ----------------------------------------------------------------------------
TEST(OpenGL_RHI240_StencilStateApplied)
{
    GLTestDevice gl;
    RequireGL(gl);

    auto vs = MakeShader(gl.device, RHIShaderStage::Vertex, kColorVS);
    auto ps = MakeShader(gl.device, RHIShaderStage::Pixel, kColorPS);
    ASSERT_TRUE(vs && ps);
    RHIPipelineStateDesc desc = ColorPipelineDesc();
    desc.depthStencil.stencilEnable = true;
    desc.depthStencil.stencilReadMask = 0x0F;
    desc.depthStencil.frontFace = {RHIStencilOp::Zero, RHIStencilOp::IncrSat, RHIStencilOp::Replace,
                                   RHICompareOp::Equal};
    desc.depthStencil.backFace = {RHIStencilOp::Keep, RHIStencilOp::Keep, RHIStencilOp::Invert, RHICompareOp::NotEqual};
    auto pso = gl.device.CreatePipelineState(desc, vs.get(), ps.get());
    ASSERT_TRUE(pso != nullptr);
    gl.device.GetImmediateCommandList()->SetPipelineState(pso.get());

    GLint value = 0;
    glGetIntegerv(GL_STENCIL_FUNC, &value);
    EXPECT_EQ(value, GL_EQUAL);
    glGetIntegerv(GL_STENCIL_VALUE_MASK, &value);
    EXPECT_EQ(value, 0x0F);
    glGetIntegerv(GL_STENCIL_FAIL, &value);
    EXPECT_EQ(value, GL_ZERO);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &value);
    EXPECT_EQ(value, GL_INCR);
    glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &value);
    EXPECT_EQ(value, GL_REPLACE);
    glGetIntegerv(GL_STENCIL_BACK_FUNC, &value);
    EXPECT_EQ(value, GL_NOTEQUAL);
    glGetIntegerv(GL_STENCIL_BACK_PASS_DEPTH_PASS, &value);
    EXPECT_EQ(value, GL_INVERT);
    EXPECT_EQ(gl.Errors(), 0);
}

// ----------------------------------------------------------------------------
// CopyTexture and samplers: non-2D targets and anisotropy=0 must not raise errors
// ----------------------------------------------------------------------------
TEST(OpenGL_RHI240_CopyTextureAndSamplerEdgeCases)
{
    GLTestDevice gl;
    RequireGL(gl);

    auto src = MakeTexture(gl.device, 2, 2, PixelFormat::R8G8B8A8_UNORM, RHITextureUsage::ShaderResource,
                           RHITextureType::Texture2DArray, 2);
    auto dst = MakeTexture(gl.device, 2, 2, PixelFormat::R8G8B8A8_UNORM, RHITextureUsage::ShaderResource,
                           RHITextureType::Texture2DArray, 2);
    ASSERT_TRUE(src && dst);
    std::vector<uint8_t> data(2 * 2 * 4, 99);
    gl.device.UpdateTexture(src.get(), data.data(), 0, 1);
    gl.device.GetImmediateCommandList()->CopyTexture(dst.get(), src.get());

    uint8_t slice1[16] = {};
    const GLuint dstName = static_cast<GLuint>(reinterpret_cast<uintptr_t>(dst->GetNativeHandle()));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTextureSubImage(dstName, 0, 0, 0, 1, 2, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, sizeof(slice1), slice1);
    EXPECT_EQ(slice1[0], 99);

    RHISamplerDesc samplerDesc;
    samplerDesc.minFilter = RHIFilterMode::Anisotropic;
    samplerDesc.magFilter = RHIFilterMode::Anisotropic;
    samplerDesc.maxAnisotropy = 0; // D3D callers commonly leave this 0 for non-aniso; GL requires >= 1
    auto sampler = gl.device.CreateSampler(samplerDesc);
    EXPECT_TRUE(sampler != nullptr);
    EXPECT_EQ(gl.Errors(), 0);
}

// ----------------------------------------------------------------------------
// Shipped GLSL: every Shaders/GLSL program compiles and links on the device,
// with stage macros (VERTEX_SHADER/FRAGMENT_SHADER) and variant defines applied.
// ----------------------------------------------------------------------------
TEST(ShaderTranslation_RHI240_ShippedGLSLCompilesAndLinks)
{
    GLTestDevice gl;
    RequireGL(gl);

    const auto dir = GLSLDir();
    ASSERT_TRUE(std::filesystem::exists(dir / "FullscreenQuad.glsl"));
    auto load = [&](const char* file) { return ReadFile(dir / file); };

    struct Program
    {
        const char* vsFile;
        const char* psFile;
        std::vector<std::string> defines;
    };
    const std::vector<Program> programs = {
        {"BasicVS.glsl", "BasicPS.glsl", {}},
        {"BasicVS.glsl", "PBRSurface.glsl", {}},
        {"FullscreenQuad.glsl", "BloomExtract.glsl", {}},
        {"FullscreenQuad.glsl", "DebugVisualize.glsl", {}},
        {"FullscreenQuad.glsl", "GaussianBlur.glsl", {}},
        {"FullscreenQuad.glsl", "GaussianBlur.glsl", {"BLUR_HORIZONTAL"}},
        {"FullscreenQuad.glsl", "PostProcess.glsl", {}},
        {"FullscreenQuad.glsl", "PostProcess.glsl", {"FXAA_PASS", "TONEMAP_REINHARD=1"}},
        {"FullscreenQuad.glsl", "PostProcess.glsl", {"GRAIN_PASS", "VIGNETTE_PASS", "CHROMATIC_PASS"}},
        {"FullscreenQuad.glsl", "SSAO.glsl", {}},
        {"EnvReflection.glsl", "EnvReflection.glsl", {}},
        {"Phong.glsl", "Phong.glsl", {}},
        {"Rim.glsl", "Rim.glsl", {}},
        {"Water.glsl", "Water.glsl", {}},
    };

    int linked = 0;
    for (const auto& program : programs)
    {
        auto vs = MakeShader(gl.device, RHIShaderStage::Vertex, load(program.vsFile), program.defines);
        auto ps = MakeShader(gl.device, RHIShaderStage::Pixel, load(program.psFile), program.defines);
        if (!vs || !ps)
        {
            std::printf("[RHI-240] compile failed: %s + %s\n", program.vsFile, program.psFile);
            EXPECT_TRUE(vs != nullptr);
            EXPECT_TRUE(ps != nullptr);
            continue;
        }
        RHIPipelineStateDesc desc;
        auto pso = gl.device.CreatePipelineState(desc, vs.get(), ps.get());
        if (!pso)
            std::printf("[RHI-240] link failed: %s + %s\n", program.vsFile, program.psFile);
        EXPECT_TRUE(pso != nullptr);
        if (pso)
            ++linked;
    }
    EXPECT_EQ(linked, static_cast<int>(programs.size()));

    // A define given as NAME=VALUE must reach the preprocessor
    const char* defineProbe = R"(#version 450 core
#if !defined(PROBE_VALUE) || PROBE_VALUE != 7
#error PROBE_VALUE missing
#endif
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(1.0); }
)";
    EXPECT_TRUE(MakeShader(gl.device, RHIShaderStage::Pixel, defineProbe, {"PROBE_VALUE=7"}) != nullptr);
    EXPECT_EQ(gl.Errors(), 0);
}

// ----------------------------------------------------------------------------
// Translation boundary: HLSL->GLSL is not integrated and must fail explicitly
// ----------------------------------------------------------------------------
TEST(ShaderTranslation_RHI240_HLSLToGLSLFailsExplicitly)
{
    ShaderCompileOptions options;
    options.stage = RHIShaderStage::Pixel;
    options.sourceLanguage = ShaderLanguage::HLSL;
    options.targetLanguage = ShaderLanguage::GLSL;
    options.sourceCode = "float4 main() : SV_Target { return float4(1, 0, 0, 1); }";
    const ShaderCompileResult result = CompileShader(options);
    EXPECT_FALSE(result.success);
    EXPECT_STR_CONTAINS(result.errorMessage, "SPIRV-Cross");
}

#endif // SPARK_OPENGL_SUPPORT
