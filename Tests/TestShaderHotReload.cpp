// TestShaderHotReload.cpp - Tests for Spark::Graphics::ShaderHotReload
#include "TestFramework.h"
#include "Graphics/ShaderHotReload.h"

#include <filesystem>
#include <fstream>
#include <string>

// Helper: create a temporary directory with shader files for testing
static std::string CreateTempShaderDir()
{
    std::string dir = "test_shaders_tmp";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    // These files are really compiled now: the reload path invokes the shader
    // compiler and GetReloadCount() only advances on a successful compile, so a
    // comment-only body would have no entry point and the reload-count
    // assertions below would go red. ShaderHotReload::ClassifyShader defaults
    // all three stems to Vertex (none of them ends in _VS/_PS/_GS), so every
    // file carries the same vs_5_0-compatible body with a "main" entry point.
    static constexpr const char* kVertexShaderSource = "struct VSOutput { float4 position : SV_Position; };\n"
                                                       "VSOutput main(float3 position : POSITION)\n"
                                                       "{\n"
                                                       "    VSOutput output;\n"
                                                       "    output.position = float4(position, 1.0f);\n"
                                                       "    return output;\n"
                                                       "}\n";
    std::ofstream(dir + "/BasicVS.hlsl") << kVertexShaderSource;
    std::ofstream(dir + "/BasicPS.hlsl") << kVertexShaderSource;
    std::ofstream(dir + "/ShadowGS.hlsl") << kVertexShaderSource;

    return dir;
}

// Helper: remove the temporary directory
static void CleanupTempShaderDir(const std::string& dir)
{
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ============================================================================
// Initialize
// ============================================================================

TEST(ShaderHotReload_Initialize_ValidDirectory)
{
    auto dir = CreateTempShaderDir();
    auto& hr = Spark::Graphics::ShaderHotReload::GetInstance();
    hr.Initialize(dir);

    EXPECT_TRUE(hr.IsEnabled());
    EXPECT_TRUE(hr.IsWatching());
    EXPECT_GT(hr.GetWatchedShaderCount(), 0u);

    hr.Shutdown();
    CleanupTempShaderDir(dir);
}

// ============================================================================
// Poll interval
// ============================================================================

TEST(ShaderHotReload_SetPollInterval)
{
    auto& hr = Spark::Graphics::ShaderHotReload::GetInstance();
    hr.Initialize(".");
    hr.SetPollInterval(2.0f);
    EXPECT_NEAR(hr.GetPollInterval(), 2.0f, 0.001f);
    hr.Shutdown();
}

TEST(ShaderHotReload_SetPollInterval_ClampMinimum)
{
    auto& hr = Spark::Graphics::ShaderHotReload::GetInstance();
    hr.Initialize(".");
    hr.SetPollInterval(0.01f);
    EXPECT_GE(hr.GetPollInterval(), 0.05f);
    hr.Shutdown();
}

// ============================================================================
// Enable/disable
// ============================================================================

TEST(ShaderHotReload_SetEnabled)
{
    auto& hr = Spark::Graphics::ShaderHotReload::GetInstance();
    hr.Initialize(".");
    EXPECT_TRUE(hr.IsEnabled());

    hr.SetEnabled(false);
    EXPECT_FALSE(hr.IsEnabled());

    hr.SetEnabled(true);
    EXPECT_TRUE(hr.IsEnabled());

    hr.Shutdown();
}

// ============================================================================
// Watched shader count
// ============================================================================

TEST(ShaderHotReload_GetWatchedShaderCount)
{
    auto dir = CreateTempShaderDir();
    auto& hr = Spark::Graphics::ShaderHotReload::GetInstance();
    hr.Initialize(dir);

    // We created 3 .hlsl files
    EXPECT_EQ(hr.GetWatchedShaderCount(), 3u);

    hr.Shutdown();
    CleanupTempShaderDir(dir);
}

// ============================================================================
// ForceReloadAll
// ============================================================================

TEST(ShaderHotReload_ForceReloadAll)
{
    auto dir = CreateTempShaderDir();
    auto& hr = Spark::Graphics::ShaderHotReload::GetInstance();
    hr.Initialize(dir);

    uint32_t reloadsBefore = hr.GetReloadCount();
    hr.ForceReloadAll();
    uint32_t reloadsAfter = hr.GetReloadCount();

    // Each shader file should have been reloaded
    EXPECT_GT(reloadsAfter, reloadsBefore);
    EXPECT_EQ(reloadsAfter - reloadsBefore, hr.GetWatchedShaderCount());

    hr.Shutdown();
    CleanupTempShaderDir(dir);
}

// ============================================================================
// OnShaderReloaded callback
// ============================================================================

TEST(ShaderHotReload_OnShaderReloaded_Callback)
{
    auto dir = CreateTempShaderDir();
    auto& hr = Spark::Graphics::ShaderHotReload::GetInstance();
    hr.Initialize(dir);

    int callbackCount = 0;
    hr.OnShaderReloaded(
        [&](const Spark::Graphics::ShaderReloadEvent& event)
        {
            ++callbackCount;
            // The file exists, so success should be true
        });

    hr.ForceReloadAll();
    EXPECT_EQ(callbackCount, static_cast<int>(hr.GetWatchedShaderCount()));

    hr.Shutdown();
    CleanupTempShaderDir(dir);
}

// ============================================================================
// Console status
// ============================================================================

TEST(ShaderHotReload_Console_GetStatus)
{
    auto dir = CreateTempShaderDir();
    auto& hr = Spark::Graphics::ShaderHotReload::GetInstance();
    hr.Initialize(dir);

    std::string status = hr.Console_GetStatus();
    EXPECT_STR_CONTAINS(status, "ShaderHotReload");
    EXPECT_STR_CONTAINS(status, "enabled");

    hr.Shutdown();
    CleanupTempShaderDir(dir);
}
