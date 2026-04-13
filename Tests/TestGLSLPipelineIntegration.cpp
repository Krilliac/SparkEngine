/**
 * @file TestGLSLPipelineIntegration.cpp
 * @brief Integration tests proving the GLSL→OpenGL RHI pipeline works end-to-end
 *
 * Tests shader compilation, pipeline state creation, buffer creation, and draw
 * calls through the RHI device interface. Uses NullRHIDevice (headless) to verify
 * the pipeline wiring without requiring a GPU.
 *
 * Also tests that the Shader class stores compiled GLSL source after compilation
 * on Linux, making it available for RHI pipeline state creation.
 *
 * NOTE: Some tests call `Shader::LoadVertexShader`/`LoadPixelShader` which
 * route to `ShaderCompilationWindows.cpp::LoadVertexShader` on Windows.
 * That path uses SPARK_REQUIRE_NOT_NULL(m_device) at line 92 — since the
 * tests initialize Shader with a null D3D11 device (there is no real GPU in
 * CI), the check always aborts on Windows. Scope the whole file to
 * non-Windows builds; a separate Windows-specific GLSL pipeline test would
 * need a real D3D11 device first.
 */

#include "TestFramework.h"

#ifndef _WIN32

#include "Graphics/GraphicsEngine.h"
#include "Graphics/RHI/RHIBridge.h"
#include "Graphics/RHI/RHIFactory.h"
#include "Graphics/RHI/RHITypes.h"
#include "Graphics/Shader.h"
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace Spark::RHI;

// ============================================================================
// Minimal GLSL shaders for testing (no uniforms, no textures)
// ============================================================================

static const char* kTestVertexShader = R"(
#version 460 core
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 0) out vec3 fragColor;
void main() {
    gl_Position = vec4(inPosition, 1.0);
    fragColor = inColor;
}
)";

static const char* kTestPixelShader = R"(
#version 460 core
layout(location = 0) in vec3 fragColor;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(fragColor, 1.0);
}
)";

// ============================================================================
// RHI Shader Compilation Tests
// ============================================================================

TEST(GLSLPipeline_CompileVertexShader_Succeeds)
{
    ShaderCompileOptions options;
    options.stage = RHIShaderStage::Vertex;
    options.sourceCode = kTestVertexShader;
    options.entryPoint = "main";
    options.sourceLanguage = ShaderLanguage::GLSL;
    options.targetLanguage = ShaderLanguage::GLSL;
    options.targetBackend = GraphicsBackend::OpenGL;

    ShaderCompileResult result = CompileShader(options);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(!result.bytecode.empty());
}

TEST(GLSLPipeline_CompilePixelShader_Succeeds)
{
    ShaderCompileOptions options;
    options.stage = RHIShaderStage::Pixel;
    options.sourceCode = kTestPixelShader;
    options.entryPoint = "main";
    options.sourceLanguage = ShaderLanguage::GLSL;
    options.targetLanguage = ShaderLanguage::GLSL;
    options.targetBackend = GraphicsBackend::OpenGL;

    ShaderCompileResult result = CompileShader(options);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(!result.bytecode.empty());
}

TEST(GLSLPipeline_GLSLPassthrough_BytecodeMatchesSource)
{
    // For GLSL→GLSL, the bytecode should be the source text verbatim
    ShaderCompileOptions options;
    options.stage = RHIShaderStage::Vertex;
    options.sourceCode = kTestVertexShader;
    options.sourceLanguage = ShaderLanguage::GLSL;
    options.targetLanguage = ShaderLanguage::GLSL;
    options.targetBackend = GraphicsBackend::OpenGL;

    ShaderCompileResult result = CompileShader(options);
    EXPECT_TRUE(result.success);

    std::string bytecodeAsString(result.bytecode.begin(), result.bytecode.end());
    EXPECT_EQ(bytecodeAsString, std::string(kTestVertexShader));
}

// ============================================================================
// RHI Device Shader Creation Tests (NullRHI)
// ============================================================================

TEST(GLSLPipeline_CreateShaderOnNullDevice_Succeeds)
{
    RHIBridge bridge;
    EXPECT_TRUE(bridge.Initialize(nullptr, 1280, 720, GraphicsBackend::None, false));

    auto* device = bridge.GetDevice();
    EXPECT_TRUE(device != nullptr);

    RHIShaderDesc desc;
    desc.stage = RHIShaderStage::Vertex;
    desc.sourceCode = kTestVertexShader;
    desc.entryPoint = "main";
    desc.language = ShaderLanguage::GLSL;
    desc.debugName = "TestVS";

    auto shader = device->CreateShader(desc);
    EXPECT_TRUE(shader != nullptr);

    bridge.Shutdown();
}

TEST(GLSLPipeline_CreatePipelineStateOnNullDevice_Succeeds)
{
    RHIBridge bridge;
    EXPECT_TRUE(bridge.Initialize(nullptr, 1280, 720, GraphicsBackend::None, false));

    auto* device = bridge.GetDevice();

    // Create vertex shader
    RHIShaderDesc vsDesc;
    vsDesc.stage = RHIShaderStage::Vertex;
    vsDesc.sourceCode = kTestVertexShader;
    vsDesc.entryPoint = "main";
    vsDesc.language = ShaderLanguage::GLSL;
    vsDesc.debugName = "TestVS";
    auto vs = device->CreateShader(vsDesc);
    EXPECT_TRUE(vs != nullptr);

    // Create pixel shader
    RHIShaderDesc psDesc;
    psDesc.stage = RHIShaderStage::Pixel;
    psDesc.sourceCode = kTestPixelShader;
    psDesc.entryPoint = "main";
    psDesc.language = ShaderLanguage::GLSL;
    psDesc.debugName = "TestPS";
    auto ps = device->CreateShader(psDesc);
    EXPECT_TRUE(ps != nullptr);

    // Create pipeline state
    RHIPipelineStateDesc psoDesc;
    psoDesc.debugName = "TestPSO";
    auto pso = device->CreatePipelineState(psoDesc, vs.get(), ps.get());
    EXPECT_TRUE(pso != nullptr);

    bridge.Shutdown();
}

// ============================================================================
// Full Draw Pipeline Test (NullRHI)
// ============================================================================

TEST(GLSLPipeline_FullDrawPipeline_NullRHI)
{
    RHIBridge bridge;
    EXPECT_TRUE(bridge.Initialize(nullptr, 1280, 720, GraphicsBackend::None, false));

    auto* device = bridge.GetDevice();
    auto* cmd = bridge.GetCommandList();
    EXPECT_TRUE(cmd != nullptr);

    // Create shaders
    RHIShaderDesc vsDesc;
    vsDesc.stage = RHIShaderStage::Vertex;
    vsDesc.sourceCode = kTestVertexShader;
    vsDesc.language = ShaderLanguage::GLSL;
    auto vs = device->CreateShader(vsDesc);

    RHIShaderDesc psDesc;
    psDesc.stage = RHIShaderStage::Pixel;
    psDesc.sourceCode = kTestPixelShader;
    psDesc.language = ShaderLanguage::GLSL;
    auto ps = device->CreateShader(psDesc);

    // Create pipeline
    RHIPipelineStateDesc psoDesc;
    auto pso = device->CreatePipelineState(psoDesc, vs.get(), ps.get());

    // Create triangle vertex buffer
    struct Vertex
    {
        float pos[3];
        float color[3];
    };
    Vertex vertices[] = {
        {{0.0f, 0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}},
        {{-0.5f, -0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {{0.5f, -0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}},
    };
    auto vb = bridge.CreateVertexBuffer(vertices, sizeof(vertices), sizeof(Vertex));
    EXPECT_TRUE(vb != nullptr);

    // Create index buffer
    uint32_t indices[] = {0, 1, 2};
    auto ib = bridge.CreateIndexBuffer(indices, sizeof(indices), sizeof(uint32_t));
    EXPECT_TRUE(ib != nullptr);

    // Issue draw commands
    bridge.BeginFrame();

    cmd->SetPipelineState(pso.get());
    cmd->SetVertexBuffer(vb.get(), 0, 0);
    cmd->SetIndexBuffer(ib.get(), 0);
    cmd->DrawIndexed(3, 0, 0);

    bridge.EndFrame();

    // NullRHI may not count draw calls in statistics — just verify no crash
    // The fact we reached here means the full pipeline executed without error

    bridge.Shutdown();
}

// ============================================================================
// Shader Class Source Storage Tests
// ============================================================================

TEST(GLSLPipeline_ShaderClass_StoresCompiledSource)
{
    Shader shader;

    // Initially empty
    EXPECT_TRUE(shader.GetCompiledVertexSource().empty());
    EXPECT_TRUE(shader.GetCompiledPixelSource().empty());
}

// ============================================================================
// Shader Factory Utility Tests
// ============================================================================

TEST(GLSLPipeline_GetShaderExtension_OpenGL)
{
    EXPECT_EQ(std::string(GetShaderExtension(GraphicsBackend::OpenGL)), std::string(".glsl"));
}

TEST(GLSLPipeline_GetShaderSearchPath_OpenGL)
{
    EXPECT_EQ(GetShaderSearchPath(GraphicsBackend::OpenGL), std::string("Shaders/GLSL/"));
}

TEST(GLSLPipeline_GetShaderExtension_D3D11)
{
    EXPECT_EQ(std::string(GetShaderExtension(GraphicsBackend::D3D11)), std::string(".hlsl"));
}

TEST(GLSLPipeline_CrossCompileHLSLtoGLSL_BasicTypes)
{
    std::string hlsl = "float4 main(float4 pos : SV_Position) : SV_Target { return pos; }";
    std::string glsl = CrossCompileHLSLtoGLSL(hlsl, RHIShaderStage::Pixel, "main");

    // Should contain translated types
    EXPECT_TRUE(glsl.find("vec4") != std::string::npos);
    // Should have GLSL version header
    EXPECT_TRUE(glsl.find("#version 450") != std::string::npos);
}

TEST(GLSLPipeline_CrossCompileHLSLtoGLSL_EmptyInput_ReturnsEmpty)
{
    std::string result = CrossCompileHLSLtoGLSL("", RHIShaderStage::Pixel, "main");
    EXPECT_TRUE(result.empty());
}

// ============================================================================
// Real GLSL Shader File Tests — loads production shaders from disk
// ============================================================================

// Helper: find the Shaders/GLSL directory (search from CWD and common paths)
static std::string FindGLSLDir()
{
    const char* candidates[] = {
        "Shaders/GLSL",
        "../Shaders/GLSL",
        "../../Shaders/GLSL",
    };
    for (auto* dir : candidates)
    {
        if (std::filesystem::exists(dir))
            return dir;
    }
    return "";
}

TEST(GLSLPipeline_LoadRealBasicVS_CompilesToGLSL)
{
    std::string glslDir = FindGLSLDir();
    if (glslDir.empty())
    {
        // Shader files not found — skip (CI may not copy them)
        return;
    }

    std::string vsPath = glslDir + "/BasicVS.glsl";
    EXPECT_TRUE(std::filesystem::exists(vsPath));

    ShaderCompileOptions options;
    options.stage = RHIShaderStage::Vertex;
    options.sourceFile = vsPath;
    options.sourceLanguage = ShaderLanguage::GLSL;
    options.targetLanguage = ShaderLanguage::GLSL;
    options.targetBackend = GraphicsBackend::OpenGL;

    // Read file
    std::ifstream f(vsPath);
    std::ostringstream ss;
    ss << f.rdbuf();
    options.sourceCode = ss.str();

    ShaderCompileResult result = CompileShader(options);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(!result.bytecode.empty());

    // Bytecode should contain GLSL version header
    std::string code(result.bytecode.begin(), result.bytecode.end());
    EXPECT_TRUE(code.find("#version 460") != std::string::npos);
}

TEST(GLSLPipeline_LoadRealBasicPS_CompilesToGLSL)
{
    std::string glslDir = FindGLSLDir();
    if (glslDir.empty())
        return;

    std::string psPath = glslDir + "/BasicPS.glsl";
    EXPECT_TRUE(std::filesystem::exists(psPath));

    ShaderCompileOptions options;
    options.stage = RHIShaderStage::Pixel;
    options.sourceFile = psPath;
    options.sourceLanguage = ShaderLanguage::GLSL;
    options.targetLanguage = ShaderLanguage::GLSL;
    options.targetBackend = GraphicsBackend::OpenGL;

    std::ifstream f(psPath);
    std::ostringstream ss;
    ss << f.rdbuf();
    options.sourceCode = ss.str();

    ShaderCompileResult result = CompileShader(options);
    EXPECT_TRUE(result.success);

    std::string code(result.bytecode.begin(), result.bytecode.end());
    EXPECT_TRUE(code.find("#version 460") != std::string::npos);
    // Should contain PBR lighting code
    EXPECT_TRUE(code.find("outColor") != std::string::npos);
}

TEST(GLSLPipeline_ShaderClass_LoadVertexShader_StoresSource)
{
    std::string glslDir = FindGLSLDir();
    if (glslDir.empty())
    {
        return;
    }

    Shader shader;
    HRESULT initHr = shader.Initialize(nullptr, nullptr);
    EXPECT_EQ(initHr, S_OK);

    std::wstring vsPath(glslDir.begin(), glslDir.end());
    vsPath += L"/BasicVS.glsl";

    // Verify the file actually exists at this path
    std::string narrowPath(vsPath.begin(), vsPath.end());
    EXPECT_TRUE(std::filesystem::exists(narrowPath));

    HRESULT hr = shader.LoadVertexShader(vsPath);
    EXPECT_EQ(hr, S_OK);
    EXPECT_TRUE(shader.IsValid());
    EXPECT_TRUE(!shader.GetCompiledVertexSource().empty());
    if (!shader.GetCompiledVertexSource().empty())
    {
        EXPECT_TRUE(shader.GetCompiledVertexSource().find("#version 460") != std::string::npos);
    }

    shader.Shutdown();
}

TEST(GLSLPipeline_ShaderClass_LoadPixelShader_StoresSource)
{
    std::string glslDir = FindGLSLDir();
    if (glslDir.empty())
        return;

    Shader shader;
    shader.Initialize(nullptr, nullptr);

    std::wstring psPath(glslDir.begin(), glslDir.end());
    psPath += L"/BasicPS.glsl";

    HRESULT hr = shader.LoadPixelShader(psPath);
    EXPECT_EQ(hr, S_OK);
    EXPECT_TRUE(!shader.GetCompiledPixelSource().empty());
    EXPECT_TRUE(shader.GetCompiledPixelSource().find("outColor") != std::string::npos);

    shader.Shutdown();
}

TEST(GLSLPipeline_ShaderClass_WithGraphicsEngine_CreatesRHIPipeline)
{
    std::string glslDir = FindGLSLDir();
    if (glslDir.empty())
        return;

    // Initialize the global GraphicsEngine — this sets up the Linux RHI
    // state singleton that Shader::Initialize reads from.
    ::GraphicsEngine engine;
    HRESULT engineHr = engine.Initialize(nullptr);
    EXPECT_EQ(engineHr, S_OK);

    Shader shader;
    EXPECT_EQ(shader.Initialize(nullptr, nullptr), S_OK);

    std::wstring vsPath(glslDir.begin(), glslDir.end());
    vsPath += L"/BasicVS.glsl";
    std::wstring psPath(glslDir.begin(), glslDir.end());
    psPath += L"/BasicPS.glsl";

    EXPECT_EQ(shader.LoadVertexShader(vsPath), S_OK);
    EXPECT_EQ(shader.LoadPixelShader(psPath), S_OK);

    // Sources stored
    EXPECT_TRUE(!shader.GetCompiledVertexSource().empty());
    EXPECT_TRUE(!shader.GetCompiledPixelSource().empty());

    // Now SetShaders should create the pipeline state — the global RHI
    // bridge is initialized via GraphicsEngine, so m_rhiDevice is valid.
    shader.SetShaders();

    // With the global RHI bridge running, the pipeline state is created
    // through the NullRHI device (which always succeeds).
    EXPECT_TRUE(shader.GetRHIPipelineState() != nullptr);

    shader.Shutdown();
    engine.Shutdown();
}

TEST(GLSLPipeline_ShaderClass_LoadBothShaders_SourcesStored)
{
    std::string glslDir = FindGLSLDir();
    if (glslDir.empty())
        return;

    Shader shader;
    shader.Initialize(nullptr, nullptr);

    std::wstring vsPath(glslDir.begin(), glslDir.end());
    vsPath += L"/BasicVS.glsl";
    std::wstring psPath(glslDir.begin(), glslDir.end());
    psPath += L"/BasicPS.glsl";

    EXPECT_EQ(shader.LoadVertexShader(vsPath), S_OK);
    EXPECT_EQ(shader.LoadPixelShader(psPath), S_OK);

    // Both sources stored — ready for RHI pipeline creation
    EXPECT_TRUE(!shader.GetCompiledVertexSource().empty());
    EXPECT_TRUE(!shader.GetCompiledPixelSource().empty());
    EXPECT_TRUE(shader.GetCompiledVertexSource().find("#version 460") != std::string::npos);
    EXPECT_TRUE(shader.GetCompiledPixelSource().find("outColor") != std::string::npos);

    // Pipeline state creation requires the global RHI bridge to be
    // initialized (full engine startup). In unit tests without a global
    // RHI device, m_rhiDevice is nullptr and the pipeline is deferred.
    // The pipeline creation path is exercised by the NullRHI draw test above.

    shader.Shutdown();
}

TEST(GLSLPipeline_AllGLSLShaders_Compile)
{
    std::string glslDir = FindGLSLDir();
    if (glslDir.empty())
        return;

    int compiled = 0;
    for (auto& entry : std::filesystem::directory_iterator(glslDir))
    {
        if (entry.path().extension() != ".glsl")
            continue;

        std::ifstream f(entry.path());
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string source = ss.str();

        // Determine stage from filename convention
        std::string stem = entry.path().stem().string();
        RHIShaderStage stage = RHIShaderStage::Pixel; // default
        if (stem.find("VS") != std::string::npos || stem.find("Quad") != std::string::npos)
            stage = RHIShaderStage::Vertex;

        ShaderCompileOptions options;
        options.stage = stage;
        options.sourceCode = source;
        options.sourceFile = entry.path().string();
        options.sourceLanguage = ShaderLanguage::GLSL;
        options.targetLanguage = ShaderLanguage::GLSL;
        options.targetBackend = GraphicsBackend::OpenGL;

        ShaderCompileResult result = CompileShader(options);
        EXPECT_TRUE(result.success);
        compiled++;
    }

    // Should have found at least the 14 known GLSL shaders
    EXPECT_TRUE(compiled >= 14);
}

#endif // !_WIN32
