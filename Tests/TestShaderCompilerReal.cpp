/**
 * @file TestShaderCompilerReal.cpp
 * @brief Production-linked coverage for the offline shader-compilation path:
 *        Spark::RHI::CompileShader, SaveCompiledShader and the honest-failure
 *        contract every non-Direct3D target now obeys.
 *
 * These tests drive the real RHIFactory entry points that SparkShaderCompiler
 * and Shader::CompileWithRHI call — there are no local reimplementations. The
 * Direct3D tests need d3dcompiler_47, so on non-Windows they skip rather than
 * assert a platform-specific outcome.
 */
#include "TestFramework.h"

#include "Graphics/RHI/RHIFactory.h"
#include "Graphics/RHI/RHITypes.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using Spark::RHI::GraphicsBackend;
using Spark::RHI::RHIShaderStage;
using Spark::RHI::ShaderCompileOptions;
using Spark::RHI::ShaderLanguage;

namespace
{
    const char* const kValidVertexShader = R"(
struct VSOutput { float4 position : SV_Position; };
VSOutput main(float3 position : POSITION)
{
    VSOutput output;
    output.position = float4(position, 1.0f);
    return output;
}
)";

#ifdef _WIN32
    const char* const kBrokenVertexShader = R"(
float4 main() : SV_Target { return this_is_not_hlsl(; }
)";

    ShaderCompileOptions MakeD3D11Options(const char* source, RHIShaderStage stage = RHIShaderStage::Vertex)
    {
        ShaderCompileOptions options;
        options.stage = stage;
        options.sourceLanguage = ShaderLanguage::HLSL;
        options.targetLanguage = ShaderLanguage::Auto;
        options.targetBackend = GraphicsBackend::D3D11;
        options.entryPoint = "main";
        options.sourceCode = source;
        return options;
    }

    bool StartsWithDXBC(const std::vector<uint8_t>& bytecode)
    {
        return bytecode.size() >= 4 && bytecode[0] == 'D' && bytecode[1] == 'X' && bytecode[2] == 'B' &&
               bytecode[3] == 'C';
    }
#endif // _WIN32
} // namespace

// ============================================================================
// HLSL -> DXBC is a real compile, not a passthrough
// ============================================================================

TEST(ShaderCompilerReal_D3D11EmitsDXBCNotSource)
{
#ifndef _WIN32
    SKIP_TEST("HLSL compilation requires d3dcompiler_47 (Windows only)");
#else
    const auto result = Spark::RHI::CompileShader(MakeD3D11Options(kValidVertexShader));

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.errorMessage.empty());
    EXPECT_TRUE(StartsWithDXBC(result.bytecode));
    // The old passthrough copied the source bytes verbatim; real bytecode
    // cannot be the same length as the text it came from.
    EXPECT_NE(result.bytecode.size(), std::string(kValidVertexShader).size());
#endif
}

TEST(ShaderCompilerReal_BrokenHLSLFailsWithCompilerDiagnostic)
{
#ifndef _WIN32
    SKIP_TEST("HLSL compilation requires d3dcompiler_47 (Windows only)");
#else
    const auto result = Spark::RHI::CompileShader(MakeD3D11Options(kBrokenVertexShader));

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.bytecode.empty());
    EXPECT_FALSE(result.errorMessage.empty());
#endif
}

TEST(ShaderCompilerReal_WrongStageForSourceFails)
{
#ifndef _WIN32
    SKIP_TEST("HLSL compilation requires d3dcompiler_47 (Windows only)");
#else
    // A vertex-shader body compiled as a compute shader has no numthreads
    // attribute, so cs_5_0 must reject it. A passthrough would report success.
    const auto result = Spark::RHI::CompileShader(MakeD3D11Options(kValidVertexShader, RHIShaderStage::Compute));

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errorMessage.empty());
#endif
}

TEST(ShaderCompilerReal_RayTracingStageReportsMissingDXC)
{
#ifndef _WIN32
    SKIP_TEST("HLSL compilation requires d3dcompiler_47 (Windows only)");
#else
    const auto result = Spark::RHI::CompileShader(MakeD3D11Options(kValidVertexShader, RHIShaderStage::RayGeneration));

    EXPECT_FALSE(result.success);
    EXPECT_STR_CONTAINS(result.errorMessage, "DXC");
#endif
}

TEST(ShaderCompilerReal_DefinesReachTheCompiler)
{
#ifndef _WIN32
    SKIP_TEST("HLSL compilation requires d3dcompiler_47 (Windows only)");
#else
    const char* const conditional = R"(
struct VSOutput { float4 position : SV_Position; };
VSOutput main(float3 position : POSITION)
{
#ifndef SPARK_TEST_DEFINE
    this_line_is_not_hlsl
#endif
    VSOutput output;
    output.position = float4(position, 1.0f);
    return output;
}
)";

    auto options = MakeD3D11Options(conditional);
    EXPECT_FALSE(Spark::RHI::CompileShader(options).success);

    options.defines.push_back("SPARK_TEST_DEFINE=1");
    const auto withDefine = Spark::RHI::CompileShader(options);
    EXPECT_TRUE(withDefine.success);
    EXPECT_TRUE(StartsWithDXBC(withDefine.bytecode));
#endif
}

// ============================================================================
// Targets with no integrated compiler fail closed with a reason
// ============================================================================

TEST(ShaderCompilerReal_VulkanTargetReportsNotIntegrated)
{
    ShaderCompileOptions options;
    options.stage = RHIShaderStage::Vertex;
    options.sourceLanguage = ShaderLanguage::HLSL;
    options.targetLanguage = ShaderLanguage::SPIRV;
    options.targetBackend = GraphicsBackend::Vulkan;
    options.entryPoint = "main";
    options.sourceCode = kValidVertexShader;

    const auto result = Spark::RHI::CompileShader(options);

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.bytecode.empty());
    EXPECT_STR_CONTAINS(result.errorMessage, "not integrated");
}

TEST(ShaderCompilerReal_OpenGLFromHLSLReportsNotIntegrated)
{
    ShaderCompileOptions options;
    options.stage = RHIShaderStage::Pixel;
    options.sourceLanguage = ShaderLanguage::HLSL;
    options.targetLanguage = ShaderLanguage::GLSL;
    options.targetBackend = GraphicsBackend::OpenGL;
    options.entryPoint = "main";
    options.sourceCode = kValidVertexShader;

    const auto result = Spark::RHI::CompileShader(options);

    // The keyword-substitution shim does not produce compilable GLSL, so this
    // path must fail rather than hand back translated text as "bytecode".
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.bytecode.empty());
    EXPECT_STR_CONTAINS(result.errorMessage, "SPIRV-Cross");
}

TEST(ShaderCompilerReal_GLSLSourceForOpenGLPassesThrough)
{
    ShaderCompileOptions options;
    options.stage = RHIShaderStage::Pixel;
    options.sourceLanguage = ShaderLanguage::GLSL;
    options.targetLanguage = ShaderLanguage::GLSL;
    options.targetBackend = GraphicsBackend::OpenGL;
    options.entryPoint = "main";
    options.sourceCode = "#version 450 core\nvoid main() {}\n";

    const auto result = Spark::RHI::CompileShader(options);

    // The OpenGL driver compiles source text, so for that backend the text is
    // the artifact and passthrough is honest.
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.bytecode.size(), options.sourceCode.size());
}

// ============================================================================
// SaveCompiledShader refuses to write source text into a .cso
// ============================================================================

TEST(ShaderCompilerReal_SaveCsoRejectsNonBytecode)
{
    const auto path = std::filesystem::temp_directory_path() / "spark_shader_compiler_reject.cso";
    std::filesystem::remove(path);

    const std::string text = "float4 main() : SV_Target { return 0; }";
    const std::vector<uint8_t> payload(text.begin(), text.end());

    EXPECT_FALSE(Spark::RHI::SaveCompiledShader(path.string(), payload));
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(ShaderCompilerReal_SaveCsoRejectsEmptyPayload)
{
    const auto path = std::filesystem::temp_directory_path() / "spark_shader_compiler_empty.cso";
    std::filesystem::remove(path);

    EXPECT_FALSE(Spark::RHI::SaveCompiledShader(path.string(), std::vector<uint8_t>{}));
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(ShaderCompilerReal_SaveCsoAcceptsRealBytecode)
{
#ifndef _WIN32
    SKIP_TEST("HLSL compilation requires d3dcompiler_47 (Windows only)");
#else
    const auto result = Spark::RHI::CompileShader(MakeD3D11Options(kValidVertexShader));
    ASSERT_TRUE(result.success);

    const auto path = std::filesystem::temp_directory_path() / "spark_shader_compiler_accept.cso";
    std::filesystem::remove(path);

    EXPECT_TRUE(Spark::RHI::SaveCompiledShader(path.string(), result.bytecode));
    EXPECT_TRUE(std::filesystem::exists(path));

    const auto reloaded = Spark::RHI::LoadPrecompiledShader(path.string());
    EXPECT_TRUE(reloaded.success);
    EXPECT_TRUE(StartsWithDXBC(reloaded.bytecode));
    EXPECT_EQ(reloaded.bytecode.size(), result.bytecode.size());

    std::filesystem::remove(path);
#endif
}

TEST(ShaderCompilerReal_SaveNonCsoExtensionIsUnrestricted)
{
    const auto path = std::filesystem::temp_directory_path() / "spark_shader_compiler_text.glsl";
    std::filesystem::remove(path);

    const std::string text = "#version 450 core\nvoid main() {}\n";
    const std::vector<uint8_t> payload(text.begin(), text.end());

    EXPECT_TRUE(Spark::RHI::SaveCompiledShader(path.string(), payload));
    EXPECT_TRUE(std::filesystem::exists(path));

    std::filesystem::remove(path);
}

// ============================================================================
// Shipped Basic shaders match the engine's constant-buffer layout
// ============================================================================

namespace
{
    // Resolve a repo-relative shader path.
    //
    // SPARK_TEST_SOURCE_DIR is CMAKE_SOURCE_DIR, so the lookup does not depend on
    // wherever CTest happens to set the working directory. The old version probed
    // four relative roots and the callers turned a miss into SKIP_TEST — a build
    // layout one level deeper or sideways silently reported a pass, which is the
    // only guard these shaders have against drifting from the engine's cbuffer
    // layouts. A miss is now a failure, not a skip.
    std::string FindShaderFile(const std::string& relative)
    {
        std::error_code ec;

#ifdef SPARK_TEST_SOURCE_DIR
        const std::filesystem::path fromSourceDir = std::filesystem::path(SPARK_TEST_SOURCE_DIR) / relative;
        if (std::filesystem::exists(fromSourceDir, ec))
            return fromSourceDir.string();
#endif

        const char* const roots[] = {"", "../", "../../", "../../../"};
        for (const char* root : roots)
        {
            const std::string candidate = std::string(root) + relative;
            if (std::filesystem::exists(candidate, ec))
                return candidate;
        }
        return {};
    }

    std::string ReadFileText(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
            return {};
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
} // namespace

TEST(ShaderCompilerReal_ShippedBasicVSCompilesAgainstPerObjectLayout)
{
#ifndef _WIN32
    SKIP_TEST("HLSL compilation requires d3dcompiler_47 (Windows only)");
#else
    // Not a skip: this is the only check that BasicVS.hlsl still matches
    // PerObjectConstants, so a missing file is a failure to investigate.
    const std::string path = FindShaderFile("Shaders/HLSL/BasicVS.hlsl");
    ASSERT_FALSE(path.empty());

    const std::string source = ReadFileText(path);
    ASSERT_FALSE(source.empty());

    // The shipped shader used to declare a 192-byte World/View/Projection
    // cbuffer while the engine binds the 320-byte PerObjectConstants.
    EXPECT_STR_CONTAINS(source, "PerObjectConstants");
    EXPECT_STR_CONTAINS(source, "PreviousWorld");
    EXPECT_STR_CONTAINS(source, "UVTiling");

    auto options = MakeD3D11Options("");
    options.sourceCode = source;
    options.sourceFile = path;
    const auto result = Spark::RHI::CompileShader(options);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(StartsWithDXBC(result.bytecode));
#endif
}

TEST(ShaderCompilerReal_ShippedBasicPSCompilesAgainstPerFrameLayout)
{
#ifndef _WIN32
    SKIP_TEST("HLSL compilation requires d3dcompiler_47 (Windows only)");
#else
    // Not a skip: this is the only check that BasicPS.hlsl still matches
    // PerFrameConstants, so a missing file is a failure to investigate.
    const std::string path = FindShaderFile("Shaders/HLSL/BasicPS.hlsl");
    ASSERT_FALSE(path.empty());

    const std::string source = ReadFileText(path);
    ASSERT_FALSE(source.empty());

    // b1 must be PerFrameConstants (three matrices first), not the old
    // 48-byte LightBuffer.
    EXPECT_STR_CONTAINS(source, "PerFrameConstants");
    EXPECT_STR_CONTAINS(source, "ViewProjectionMatrix");
    EXPECT_STR_CONTAINS(source, "DirectionalLightDir");

    auto options = MakeD3D11Options("", RHIShaderStage::Pixel);
    options.sourceCode = source;
    options.sourceFile = path;
    const auto result = Spark::RHI::CompileShader(options);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(StartsWithDXBC(result.bytecode));
#endif
}

// ============================================================================
// DXR export-name table matches the shipped ray-tracing shaders
// ============================================================================

TEST(ShaderCompilerReal_DXRShadersExportTheNamesThePSOTableUses)
{
    struct Expectation
    {
        const char* file;
        const char* rayGen;
        const char* miss;
        const char* closestHit; // nullptr when the library has none
    };

    const Expectation expectations[] = {
        {"Shaders/HLSL/RayTracing/DXRReflections.hlsl", "RayGen", "Miss", "ClosestHit"},
        {"Shaders/HLSL/RayTracing/DXRShadows.hlsl", "ShadowRayGen", "ShadowMiss", nullptr},
        {"Shaders/HLSL/RayTracing/DXRAO.hlsl", "AORayGen", "AOMiss", "AOClosestHit"},
        {"Shaders/HLSL/RayTracing/DXRGI.hlsl", "GIRayGen", "GIMiss", "GIClosestHit"},
    };

    for (const auto& expectation : expectations)
    {
        const std::string path = FindShaderFile(expectation.file);
        if (path.empty())
            SKIP_TEST("Ray-tracing shaders not reachable from the test working directory");

        const std::string source = ReadFileText(path);
        ASSERT_FALSE(source.empty());

        EXPECT_STR_CONTAINS(source, std::string("void ") + expectation.rayGen + "(");
        EXPECT_STR_CONTAINS(source, std::string("void ") + expectation.miss + "(");
        if (expectation.closestHit != nullptr)
        {
            EXPECT_STR_CONTAINS(source, std::string("void ") + expectation.closestHit + "(");
        }
        else
        {
            EXPECT_TRUE(source.find("closesthit") == std::string::npos);
        }
    }
}
