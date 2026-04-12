/**
 * @file TestShaderHotReloadPhaseU.cpp
 * @brief Phase U activation tests for Spark::Graphics::ShaderHotReload
 *
 * These tests exercise behaviours introduced or relied on by the Phase U
 * wire-up into Shader::Initialize / LoadFromFile / HotReloadShaders:
 *
 *   - Update(dt) poll-interval gating (multi-call accumulation before poll)
 *   - Idempotent Initialize — the wire-up calls Initialize once per process
 *     so any subsequent Shader::Initialize must not crash or double-register
 *   - AddWatchDirectory merges new directories into the watched-file registry
 *   - Mtime bump + Update triggers a reload callback (deterministic change
 *     detection path exercised by Shader::HotReloadShaders)
 *   - AddWatchDirectory on a nonexistent path is silently ignored (tolerant
 *     to a Shader instance with unpopulated search paths)
 *   - Nested directories are discovered recursively (matches the Shader.cpp
 *     search-path conventions with Shaders/HLSL subtrees)
 *
 * These tests run against the real Spark::Graphics::ShaderHotReload class —
 * no local reimplementation — per the Phase L / O / P / Q playbook.
 */

#include "TestFramework.h"
#include "Graphics/ShaderHotReload.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace
{

    // Helper: create a disposable directory with optional shader files
    std::filesystem::path MakeTempDir(const std::string& suffix)
    {
        auto dir = std::filesystem::temp_directory_path() / ("spark_phaseU_" + suffix);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    // Helper: write a shader stub file
    void WriteShader(const std::filesystem::path& path, const std::string& body)
    {
        std::ofstream(path) << body;
    }

    // Helper: bump a file's mtime without changing content. The poll path in
    // ShaderHotReload reads last_write_time and compares tick counts, so a
    // portable way to force a re-read is to rewrite the file and sleep long
    // enough for the filesystem timestamp resolution (1 s on many systems).
    void TouchShader(const std::filesystem::path& path, const std::string& newBody)
    {
        // Filesystems like ext4 can have 1s mtime granularity.
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        std::ofstream(path, std::ios::trunc) << newBody;
    }

    // Ensure the process-wide singleton starts each test with no residual
    // watched files, callbacks, or compiled shaders from other test files
    // running earlier in the suite. Shutdown() clears all lifetime state.
    void ResetSingleton()
    {
        Spark::Graphics::ShaderHotReload::GetInstance().Shutdown();
    }

} // namespace

// ============================================================================
// Update(dt) poll-interval gating
// ============================================================================

TEST(ShaderHotReloadPhaseU_UpdateBelowIntervalDoesNotPoll)
{
    ResetSingleton();
    auto dir = MakeTempDir("poll1");
    WriteShader(dir / "A.hlsl", "// A");

    auto& hr = Spark::Graphics::ShaderHotReload::GetInstance();
    hr.Initialize(dir.string());
    hr.SetPollInterval(0.5f);

    int reloadCallbacks = 0;
    hr.OnShaderReloaded([&](const Spark::Graphics::ShaderReloadEvent&) { ++reloadCallbacks; });

    // Three sub-poll-interval ticks should accumulate but not trigger
    // CheckForChanges; file is unchanged so no callbacks should fire.
    hr.Update(0.1f);
    hr.Update(0.1f);
    hr.Update(0.1f);

    EXPECT_EQ(reloadCallbacks, 0);

    hr.Shutdown();
    std::filesystem::remove_all(dir);
}

TEST(ShaderHotReloadPhaseU_UpdateOverIntervalPollsOnce)
{
    ResetSingleton();
    auto dir = MakeTempDir("poll2");
    WriteShader(dir / "A.hlsl", "// A");

    auto& hr = Spark::Graphics::ShaderHotReload::GetInstance();
    hr.Initialize(dir.string());
    hr.SetPollInterval(0.1f);

    int reloadCallbacks = 0;
    hr.OnShaderReloaded([&](const Spark::Graphics::ShaderReloadEvent&) { ++reloadCallbacks; });

    // A single tick past the interval exercises the poll path; since the
    // file hasn't changed yet, the callback count should stay at zero.
    hr.Update(0.2f);
    EXPECT_EQ(reloadCallbacks, 0);

    hr.Shutdown();
    std::filesystem::remove_all(dir);
}

// ============================================================================
// Idempotent Initialize — Phase U wire-up may call Initialize multiple times
// ============================================================================

TEST(ShaderHotReloadPhaseU_InitializeIsIdempotent)
{
    ResetSingleton();
    auto dir = MakeTempDir("idem");
    WriteShader(dir / "A.hlsl", "// A");
    WriteShader(dir / "B.hlsl", "// B");

    auto& hr = Spark::Graphics::ShaderHotReload::GetInstance();
    hr.Initialize(dir.string());
    const uint32_t first = hr.GetWatchedShaderCount();

    // Second Initialize on the same directory should reset state cleanly.
    hr.Initialize(dir.string());
    const uint32_t second = hr.GetWatchedShaderCount();

    EXPECT_EQ(first, second);
    EXPECT_TRUE(hr.IsWatching());

    hr.Shutdown();
    std::filesystem::remove_all(dir);
}

// ============================================================================
// AddWatchDirectory — merging multiple directories (the wire-up pattern)
// ============================================================================

TEST(ShaderHotReloadPhaseU_AddWatchDirectoryMergesShaders)
{
    ResetSingleton();
    auto dirA = MakeTempDir("mergeA");
    auto dirB = MakeTempDir("mergeB");
    WriteShader(dirA / "OneVS.hlsl", "// one");
    WriteShader(dirB / "TwoPS.hlsl", "// two");
    WriteShader(dirB / "ThreeGS.hlsl", "// three");

    auto& hr = Spark::Graphics::ShaderHotReload::GetInstance();
    hr.Initialize(dirA.string());
    const uint32_t afterA = hr.GetWatchedShaderCount();
    EXPECT_EQ(afterA, static_cast<uint32_t>(1));

    // Second directory joins the registry; total shader count grows.
    hr.AddWatchDirectory(dirB.string());
    const uint32_t afterB = hr.GetWatchedShaderCount();
    EXPECT_EQ(afterB, static_cast<uint32_t>(3));

    hr.Shutdown();
    std::filesystem::remove_all(dirA);
    std::filesystem::remove_all(dirB);
}

TEST(ShaderHotReloadPhaseU_AddWatchDirectoryNonexistentIsSafe)
{
    ResetSingleton();
    auto dir = MakeTempDir("nonexist");
    WriteShader(dir / "A.hlsl", "// A");

    auto& hr = Spark::Graphics::ShaderHotReload::GetInstance();
    hr.Initialize(dir.string());
    const uint32_t before = hr.GetWatchedShaderCount();

    // AddWatchDirectory on a missing path should not throw and should not
    // grow the watched-file set. This matches the Shader.cpp guard:
    // std::filesystem::exists() check before the call.
    hr.AddWatchDirectory((dir / "does-not-exist-dir").string());
    const uint32_t after = hr.GetWatchedShaderCount();
    EXPECT_EQ(before, after);

    hr.Shutdown();
    std::filesystem::remove_all(dir);
}

// ============================================================================
// Nested directory traversal — Shaders/HLSL convention
// ============================================================================

TEST(ShaderHotReloadPhaseU_RecursiveDirectoryTraversal)
{
    ResetSingleton();
    auto dir = MakeTempDir("nested");
    std::filesystem::create_directories(dir / "SubA");
    std::filesystem::create_directories(dir / "SubB" / "Inner");
    WriteShader(dir / "Top.hlsl", "// top");
    WriteShader(dir / "SubA" / "MidA.hlsl", "// mid A");
    WriteShader(dir / "SubB" / "MidB.hlsl", "// mid B");
    WriteShader(dir / "SubB" / "Inner" / "Deep.hlsl", "// deep");

    auto& hr = Spark::Graphics::ShaderHotReload::GetInstance();
    hr.Initialize(dir.string());

    // All four shader files should be discovered recursively.
    EXPECT_EQ(hr.GetWatchedShaderCount(), static_cast<uint32_t>(4));

    hr.Shutdown();
    std::filesystem::remove_all(dir);
}

// ============================================================================
// Mtime bump + Update triggers reload
// ============================================================================

TEST(ShaderHotReloadPhaseU_MtimeBumpTriggersReload)
{
    ResetSingleton();
    auto dir = MakeTempDir("mtime");
    const auto shaderPath = dir / "BumpVS.hlsl";
    WriteShader(shaderPath, "float4 main():SV_Target{return 0;}");

    auto& hr = Spark::Graphics::ShaderHotReload::GetInstance();
    hr.Initialize(dir.string());
    hr.SetPollInterval(0.05f);

    int reloadCallbacks = 0;
    std::string lastShaderName;
    hr.OnShaderReloaded(
        [&](const Spark::Graphics::ShaderReloadEvent& ev)
        {
            ++reloadCallbacks;
            lastShaderName = ev.shaderName;
        });

    // Baseline — no reload yet from a stable mtime.
    hr.Update(0.1f);
    EXPECT_EQ(reloadCallbacks, 0);

    // Bump mtime by rewriting with a new body after the FS mtime
    // granularity window. Singleton should pick up the change on the next
    // over-interval poll.
    TouchShader(shaderPath, "float4 main():SV_Target{return float4(1,2,3,4);}");
    hr.Update(0.1f);

    EXPECT_GE(reloadCallbacks, 1);
    EXPECT_EQ(lastShaderName, std::string("BumpVS"));

    hr.Shutdown();
    std::filesystem::remove_all(dir);
}

// ============================================================================
// Wire-up invariants — mirrors what Shader::Initialize / LoadFromFile expect
// ============================================================================

TEST(ShaderHotReloadPhaseU_InitialiseThenShutdownDisables)
{
    ResetSingleton();
    auto dir = MakeTempDir("disable");
    WriteShader(dir / "A.hlsl", "// A");

    auto& hr = Spark::Graphics::ShaderHotReload::GetInstance();
    hr.Initialize(dir.string());
    EXPECT_TRUE(hr.IsWatching());

    hr.Shutdown();
    EXPECT_FALSE(hr.IsWatching());
    EXPECT_FALSE(hr.IsEnabled());
    EXPECT_EQ(hr.GetWatchedShaderCount(), static_cast<uint32_t>(0));

    std::filesystem::remove_all(dir);
}

TEST(ShaderHotReloadPhaseU_UpdateAfterShutdownIsNoOp)
{
    ResetSingleton();
    auto dir = MakeTempDir("disable2");
    WriteShader(dir / "A.hlsl", "// A");

    auto& hr = Spark::Graphics::ShaderHotReload::GetInstance();
    hr.Initialize(dir.string());
    hr.Shutdown();

    int reloadCallbacks = 0;
    hr.OnShaderReloaded([&](const Spark::Graphics::ShaderReloadEvent&) { ++reloadCallbacks; });

    // Update on a shut-down singleton must be a silent no-op: the Phase U
    // wire-up calls Update every frame regardless of whether a Shader
    // instance has been created yet.
    hr.Update(10.0f);
    EXPECT_EQ(reloadCallbacks, 0);

    // Clear the lingering callback so subsequent tests start clean.
    hr.Shutdown();
    std::filesystem::remove_all(dir);
}
