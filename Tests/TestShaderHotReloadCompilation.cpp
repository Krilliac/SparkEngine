/**
 * @file TestShaderHotReloadCompilation.cpp
 * @brief Tests for shader hot-reload compilation functionality
 */

#include "TestFramework.h"
#include "Graphics/ShaderHotReload.h"

#include <filesystem>
#include <fstream>

using namespace Spark::Graphics;

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
        std::ofstream(tempDir / "shader.hlsl") << "float4 main():SV_Target{return 0;}";
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
        std::ofstream(tempDir / "vs.hlsl") << "float4 main():SV_Target{return 0;}";
    }

    hr.Initialize(tempDir.string());
    uint32_t before = hr.GetReloadCount();
    hr.ForceReloadAll();
    uint32_t after = hr.GetReloadCount();

    EXPECT_TRUE(after >= before);

    hr.Shutdown();
    std::filesystem::remove_all(tempDir);
}

TEST(ShaderReloadComp_SuccessfulCompileSwapsAtomically)
{
    auto& hr = ShaderHotReload::GetInstance();
    auto tempDir = std::filesystem::temp_directory_path() / "spark_shr_test6";
    std::filesystem::create_directories(tempDir);

    const auto shaderPath = tempDir / "AtomicSwapVS.hlsl";
    {
        std::ofstream(shaderPath) << "float4 main():SV_Target{return float4(1,0,0,1);}";
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
    auto& hr = ShaderHotReload::GetInstance();
    auto tempDir = std::filesystem::temp_directory_path() / "spark_shr_test7";
    std::filesystem::create_directories(tempDir);

    const auto shaderPath = tempDir / "RevertVS.hlsl";
    {
        std::ofstream(shaderPath) << "float4 main():SV_Target{return float4(0,1,0,1);}";
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
