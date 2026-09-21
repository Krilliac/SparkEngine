// TestShaderHotReload.cpp - Tests for Spark::Graphics::ShaderHotReload
#include "TestFramework.h"
#include "Graphics/ShaderHotReload.h"

#include <filesystem>
#include <fstream>
#include <algorithm>
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

TEST(ShaderHotReload_Initialize_UnicodePath)
{
    const auto dir = std::filesystem::temp_directory_path() / L"SparkShaderHotReload-世界";
    const auto shader = dir / L"Basic-世界.hlsl";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream(shader) << "float4 main(float3 position : POSITION) : SV_Position { return float4(position, 1.0f); }\n";

    const auto dirUtf8Value = dir.u8string();
    const std::string dirUtf8(reinterpret_cast<const char*>(dirUtf8Value.data()), dirUtf8Value.size());
    auto& hr = Spark::Graphics::ShaderHotReload::GetInstance();
    EXPECT_NO_THROW(hr.Initialize(dirUtf8));
    EXPECT_EQ(hr.GetWatchedShaderCount(), 1u);
    hr.Shutdown();
    std::filesystem::remove_all(dir, ec);
}

TEST(ShaderHotReload_UnicodeReload_PreservesUtf8PathAndClassification)
{
    struct ScopedShaderDirectory
    {
        std::filesystem::path dir = std::filesystem::temp_directory_path() / L"SparkShaderHotReload-世界-reload";
        ~ScopedShaderDirectory()
        {
            auto& hotReload = Spark::Graphics::ShaderHotReload::GetInstance();
            hotReload.Shutdown();
            std::error_code cleanupEc;
            std::filesystem::remove_all(dir, cleanupEc);
        }
    } scope;

    const auto shader = scope.dir / L"Basic-世界_VS.hlsl";
    std::error_code ec;
    std::filesystem::create_directories(scope.dir, ec);
    std::ofstream(shader) << "float4 main(float3 position : POSITION) : SV_Position { return float4(position, 1.0f); }\n";

    const auto dirUtf8Value = scope.dir.u8string();
    const std::string dirUtf8(reinterpret_cast<const char*>(dirUtf8Value.data()), dirUtf8Value.size());
    const auto expectedPathValue = shader.generic_u8string();
    const std::string expectedPath(reinterpret_cast<const char*>(expectedPathValue.data()), expectedPathValue.size());
    const auto expectedNameValue = shader.stem().generic_u8string();
    const std::string expectedName(reinterpret_cast<const char*>(expectedNameValue.data()), expectedNameValue.size());

    auto& hr = Spark::Graphics::ShaderHotReload::GetInstance();
    hr.Initialize(dirUtf8);
    EXPECT_EQ(hr.GetWatchedShaderCount(), 1u);

    std::string callbackPath;
    std::string callbackName;
    hr.OnShaderReloaded([&](const Spark::Graphics::ShaderReloadEvent& event) {
        callbackPath = event.shaderPath;
        callbackName = event.shaderName;
    });
    hr.ForceReload(expectedName);

    EXPECT_EQ(callbackPath, expectedPath);
    EXPECT_EQ(callbackName, expectedName);
    EXPECT_NE(callbackPath, std::string{});
}

TEST(ShaderHotReload_RemoveWatchDirectory_CanonicalizesEquivalentPaths)
{
    auto dir = CreateTempShaderDir();
    auto& hr = Spark::Graphics::ShaderHotReload::GetInstance();
    hr.Initialize(dir);

    const auto absolutePath = std::filesystem::absolute(std::filesystem::path(dir));
    const auto absoluteUtf8Value = absolutePath.generic_u8string();
    std::string equivalent(reinterpret_cast<const char*>(absoluteUtf8Value.data()), absoluteUtf8Value.size());
#ifdef _WIN32
    // Exercise Windows' alternate accepted separator spelling. On POSIX a
    // backslash is a literal filename character, not a directory separator.
    std::replace(equivalent.begin(), equivalent.end(), '/', '\\');
#endif

    hr.AddWatchDirectory(equivalent);
    EXPECT_TRUE(hr.IsWatching());
    hr.RemoveWatchDirectory(equivalent);
    EXPECT_FALSE(hr.IsWatching());
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

#ifdef _WIN32
    // Each shader file is really compiled (D3DCompile) and counted as a reload.
    EXPECT_GT(reloadsAfter, reloadsBefore);
    EXPECT_EQ(reloadsAfter - reloadsBefore, hr.GetWatchedShaderCount());
#else
    // Only the DXBC target has a compiler behind RHI::CompileShader; every other
    // target fails closed. A forced reload here must therefore count zero
    // successes: counting one would be exactly the silent "reload" the
    // fail-closed change removed, so this asserts the absence, not a skip.
    EXPECT_EQ(reloadsAfter, reloadsBefore);
    EXPECT_EQ(hr.GetWatchedShaderCount(), 3u);
#endif

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
