/**
 * @file TestShaderHotReloadCompilation.cpp
 * @brief Tests for shader hot-reload compilation functionality.
 *
 * The reload path routes HLSL through Spark::RHI::CompileShader, which now
 * calls d3dcompiler_47 instead of copying the source bytes through. These
 * tests therefore use HLSL that really compiles for the stage the watcher
 * infers, and assert that a syntactically broken shader fails, keeps the
 * previous binary, and does not advance the swap generation. On platforms
 * without d3dcompiler the compile path has no compiler, so the tests that
 * require a successful reload skip.
 */

#include "TestFramework.h"
#include "Graphics/ShaderHotReload.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace Spark::Graphics;

namespace
{
    // ShaderHotReload::ClassifyShader defaults every .hlsl file to the vertex
    // stage unless the stem ends in _PS/_GS/..., so these bodies must be valid
    // vs_5_0 shaders.
    const char* const kValidVertexShader = "struct VSOutput { float4 position : SV_Position; };\n"
                                           "VSOutput main(float3 position : POSITION)\n"
                                           "{ VSOutput o; o.position = float4(position, 1.0f); return o; }\n";

    const char* const kBrokenVertexShader = "float4 main() : SV_Target { return this_is_not_hlsl(; }\n";

#ifdef _WIN32
    constexpr bool kHLSLCompilerAvailable = true;
#else
    constexpr bool kHLSLCompilerAvailable = false;
#endif
} // namespace

TEST(ShaderReloadComp_InitializeAndShutdown)
{
    auto& hr = ShaderHotReload::GetInstance();
    auto tempDir = std::filesystem::temp_directory_path() / "spark_shr_test";
    std::filesystem::create_directories(tempDir);

    hr.Initialize(tempDir.string());
    EXPECT_TRUE(hr.IsWatching());
    EXPECT_TRUE(hr.IsEnabled());

    hr.Shutdown();
    EXPECT_FALSE(hr.IsEnabled());

    std::filesystem::remove_all(tempDir);
}

TEST(ShaderReloadComp_WatchedFileCount)
{
    auto& hr = ShaderHotReload::GetInstance();
    auto tempDir = std::filesystem::temp_directory_path() / "spark_shr_test2";
    std::filesystem::create_directories(tempDir);

    // Create some shader files
    {
        std::ofstream(tempDir / "test1.hlsl") << "float4 main():SV_Target{return 0;}";
        std::ofstream(tempDir / "test2.hlsl") << "float4 main():SV_Target{return 1;}";
        std::ofstream(tempDir / "notashader.txt") << "hello";
    }

    hr.Initialize(tempDir.string());

    // Should find 2 shader files (not the .txt)
    EXPECT_EQ(hr.GetWatchedShaderCount(), static_cast<uint32_t>(2));

    hr.Shutdown();
    std::filesystem::remove_all(tempDir);
}

TEST(ShaderReloadComp_ForceReloadTriggersCallbacks)
{
    auto& hr = ShaderHotReload::GetInstance();
    auto tempDir = std::filesystem::temp_directory_path() / "spark_shr_test3";
    std::filesystem::create_directories(tempDir);

    {
        std::ofstream(tempDir / "shader.hlsl") << kValidVertexShader;
    }

    hr.Initialize(tempDir.string());

    int callbackCount = 0;
    hr.OnShaderReloaded([&](const ShaderReloadEvent& ev) { callbackCount++; });

    hr.ForceReloadAll();
    EXPECT_TRUE(callbackCount > 0);

    hr.Shutdown();
    std::filesystem::remove_all(tempDir);
}

TEST(ShaderReloadComp_PollIntervalClamped)
{
    auto& hr = ShaderHotReload::GetInstance();

    hr.SetPollInterval(0.01f);
    EXPECT_TRUE(hr.GetPollInterval() >= 0.05f);

    hr.SetPollInterval(2.0f);
    EXPECT_NEAR(hr.GetPollInterval(), 2.0f, 0.001f);
}

TEST(ShaderReloadComp_EnableDisable)
{
    auto& hr = ShaderHotReload::GetInstance();
    auto tempDir = std::filesystem::temp_directory_path() / "spark_shr_test4";
    std::filesystem::create_directories(tempDir);

    hr.Initialize(tempDir.string());
    EXPECT_TRUE(hr.IsEnabled());

    hr.SetEnabled(false);
    EXPECT_FALSE(hr.IsEnabled());

    hr.SetEnabled(true);
    EXPECT_TRUE(hr.IsEnabled());

    hr.Shutdown();
    std::filesystem::remove_all(tempDir);
}

TEST(ShaderReloadComp_ConsoleStatus)
{
    auto& hr = ShaderHotReload::GetInstance();
    auto status = hr.Console_GetStatus();
    EXPECT_TRUE(status.find("ShaderHotReload") != std::string::npos);
}

TEST(ShaderReloadComp_ReloadCountIncrements)
{
    auto& hr = ShaderHotReload::GetInstance();
    auto tempDir = std::filesystem::temp_directory_path() / "spark_shr_test5";
    std::filesystem::create_directories(tempDir);

    {
        std::ofstream(tempDir / "vs.hlsl") << kValidVertexShader;
    }

    hr.Initialize(tempDir.string());
    uint32_t before = hr.GetReloadCount();
    hr.ForceReloadAll();
    uint32_t after = hr.GetReloadCount();

    // GetReloadCount only advances on a successful compile, so `after >= before`
    // would hold even if nothing compiled. Assert the exact increment where a
    // compiler exists.
    if constexpr (kHLSLCompilerAvailable)
    {
        EXPECT_EQ(after, before + 1u);
    }
    else
    {
        EXPECT_EQ(after, before);
    }

    hr.Shutdown();
    std::filesystem::remove_all(tempDir);
}

TEST(ShaderReloadComp_SuccessfulCompileSwapsAtomically)
{
    if constexpr (!kHLSLCompilerAvailable)
        SKIP_TEST("Hot-reload compilation needs d3dcompiler_47 (Windows only)");

    auto& hr = ShaderHotReload::GetInstance();
    auto tempDir = std::filesystem::temp_directory_path() / "spark_shr_test6";
    std::filesystem::create_directories(tempDir);

    const auto shaderPath = tempDir / "AtomicSwapVS.hlsl";
    {
        std::ofstream(shaderPath) << kValidVertexShader;
    }

    hr.Initialize(tempDir.string());

    ShaderReloadEvent lastEvent{};
    hr.OnShaderReloaded([&](const ShaderReloadEvent& ev) { lastEvent = ev; });
    hr.ForceReload("AtomicSwapVS");

    EXPECT_TRUE(lastEvent.success);
    EXPECT_FALSE(lastEvent.reusedPreviousBinary);
    EXPECT_TRUE(lastEvent.errorMessage.empty());
    EXPECT_TRUE(hr.HasCompiledShader("AtomicSwapVS"));
    EXPECT_EQ(hr.GetShaderSwapGeneration("AtomicSwapVS"), 1u);

    hr.Shutdown();
    std::filesystem::remove_all(tempDir);
}

TEST(ShaderReloadComp_FailedCompileKeepsPreviousShader)
{
    if constexpr (!kHLSLCompilerAvailable)
        SKIP_TEST("Hot-reload compilation needs d3dcompiler_47 (Windows only)");

    auto& hr = ShaderHotReload::GetInstance();
    auto tempDir = std::filesystem::temp_directory_path() / "spark_shr_test7";
    std::filesystem::create_directories(tempDir);

    const auto shaderPath = tempDir / "RevertVS.hlsl";
    {
        std::ofstream(shaderPath) << kValidVertexShader;
    }

    hr.Initialize(tempDir.string());
    hr.ForceReload("RevertVS");
    EXPECT_TRUE(hr.HasCompiledShader("RevertVS"));
    EXPECT_EQ(hr.GetShaderSwapGeneration("RevertVS"), 1u);

    ShaderReloadEvent failureEvent{};
    hr.OnShaderReloaded(
        [&](const ShaderReloadEvent& ev)
        {
            if (ev.shaderName == "RevertVS" && !ev.success)
                failureEvent = ev;
        });

    // Empty file is a deterministic compile failure for this pipeline.
    {
        std::ofstream(shaderPath, std::ios::trunc) << "";
    }
    hr.ForceReload("RevertVS");

    EXPECT_FALSE(failureEvent.success);
    EXPECT_TRUE(failureEvent.reusedPreviousBinary);
    EXPECT_FALSE(failureEvent.compileLog.empty());
    EXPECT_TRUE(hr.HasCompiledShader("RevertVS"));
    EXPECT_EQ(hr.GetShaderSwapGeneration("RevertVS"), 1u);

    hr.Shutdown();
    std::filesystem::remove_all(tempDir);
}

// ============================================================================
// Compilation is real: broken HLSL must fail, and a success must carry DXBC
// ============================================================================

TEST(ShaderReloadComp_InvalidHLSLFails)
{
    if constexpr (!kHLSLCompilerAvailable)
        SKIP_TEST("Hot-reload compilation needs d3dcompiler_47 (Windows only)");

    auto& hr = ShaderHotReload::GetInstance();
    auto tempDir = std::filesystem::temp_directory_path() / "spark_shr_test8";
    std::filesystem::create_directories(tempDir);

    const auto shaderPath = tempDir / "BrokenVS.hlsl";
    {
        std::ofstream(shaderPath) << kBrokenVertexShader;
    }

    hr.Initialize(tempDir.string());

    ShaderReloadEvent lastEvent{};
    hr.OnShaderReloaded([&](const ShaderReloadEvent& ev) { lastEvent = ev; });
    hr.ForceReload("BrokenVS");

    // A file that is not valid HLSL must not report a successful reload, and
    // no new binary may be published for it.
    EXPECT_FALSE(lastEvent.success);
    EXPECT_FALSE(lastEvent.errorMessage.empty());
    EXPECT_FALSE(hr.HasCompiledShader("BrokenVS"));
    EXPECT_EQ(hr.GetShaderSwapGeneration("BrokenVS"), 0u);

    hr.Shutdown();
    std::filesystem::remove_all(tempDir);
}

TEST(ShaderReloadComp_CommentOnlyShaderIsNotAReload)
{
    if constexpr (!kHLSLCompilerAvailable)
        SKIP_TEST("Hot-reload compilation needs d3dcompiler_47 (Windows only)");

    auto& hr = ShaderHotReload::GetInstance();
    auto tempDir = std::filesystem::temp_directory_path() / "spark_shr_test9";
    std::filesystem::create_directories(tempDir);

    const auto shaderPath = tempDir / "StubVS.hlsl";
    {
        std::ofstream(shaderPath) << "// vertex shader stub\n";
    }

    hr.Initialize(tempDir.string());

    ShaderReloadEvent lastEvent{};
    hr.OnShaderReloaded([&](const ShaderReloadEvent& ev) { lastEvent = ev; });
    hr.ForceReload("StubVS");

    // A file with no entry point is not a shader. The old passthrough copied
    // its bytes into m_compiledShaders and logged "Shader hot-reloaded"; a real
    // compile reports "entrypoint not found" and publishes nothing.
    EXPECT_FALSE(lastEvent.success);
    EXPECT_FALSE(lastEvent.errorMessage.empty());
    EXPECT_FALSE(hr.HasCompiledShader("StubVS"));
    EXPECT_EQ(hr.GetShaderSwapGeneration("StubVS"), 0u);
    EXPECT_EQ(hr.GetReloadCount(), 0u);

    hr.Shutdown();
    std::filesystem::remove_all(tempDir);
}
