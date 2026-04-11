/**
 * @file TestShaderDiskCachePhaseV.cpp
 * @brief Phase V activation tests for Spark::Graphics::ShaderDiskCache
 *
 * These tests exercise behaviours introduced or relied on by the Phase V
 * wire-up into Shader::Initialize / Shader::LoadShaderFromSource:
 *
 *   - GetShaderDiskCache() singleton accessor returns a stable instance
 *     (same reference on repeat calls) — required for cross-TU sharing.
 *   - Initialize creates the cache directory and marks IsInitialized().
 *   - Store + Lookup round-trip preserves bytecode, stage, target.
 *   - Hash differs when source changes (cache miss on source edit).
 *   - Hash differs when defines change (cache miss on keyword toggle).
 *   - Hash differs when stage changes (VS vs PS of identical code).
 *   - Lookup on empty cache returns nullopt.
 *   - Store refuses non-success blobs and empty bytecode.
 *   - Clear wipes all .blob files but keeps directory + initialization.
 *   - GetEntryCount / GetDiskUsage track stores and clears.
 *   - Second Initialize on same directory is idempotent (reinit-safe).
 *
 * Tests run against the real Spark::Graphics::ShaderDiskCache — no
 * local reimplementation — per the Phase L / O / P / Q / U playbook.
 */

#include "TestFramework.h"
#include "Graphics/ShaderDiskCache.h"

#include <filesystem>
#include <string>
#include <vector>

namespace
{

    // Helper: create a disposable cache directory for each test
    std::filesystem::path MakeCacheDir(const std::string& suffix)
    {
        auto dir = std::filesystem::temp_directory_path() / ("spark_phaseV_" + suffix);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    // Helper: build a trivial ShaderSource with optional overrides
    Spark::Graphics::ShaderSource MakeSource(const std::string& hlsl = "float4 main():SV_Target{return 0;}",
                                             Spark::Graphics::ShaderStage stage = Spark::Graphics::ShaderStage::Vertex,
                                             std::vector<std::string> defines = {})
    {
        Spark::Graphics::ShaderSource s;
        s.hlslCode = hlsl;
        s.entryPoint = "main";
        s.stage = stage;
        s.defines = std::move(defines);
        return s;
    }

    // Helper: build a trivial CompiledShaderBlob with a given byte
    Spark::Graphics::CompiledShaderBlob MakeBlob(uint8_t fill, size_t size = 32)
    {
        Spark::Graphics::CompiledShaderBlob blob;
        blob.bytecode.assign(size, fill);
        blob.target = Spark::Graphics::ShaderTarget::DXBC;
        blob.stage = Spark::Graphics::ShaderStage::Vertex;
        blob.entryPoint = "main";
        blob.success = true;
        return blob;
    }

    // Reset the singleton between tests by calling Shutdown() on it.
    void ResetDiskCache()
    {
        Spark::Graphics::GetShaderDiskCache().Shutdown();
    }

} // namespace

// ============================================================================
// Singleton accessor stability
// ============================================================================

TEST(ShaderDiskCachePhaseV_SingletonReturnsSameInstance)
{
    auto& a = Spark::Graphics::GetShaderDiskCache();
    auto& b = Spark::Graphics::GetShaderDiskCache();
    EXPECT_TRUE(&a == &b);
}

// ============================================================================
// Initialize / Shutdown
// ============================================================================

TEST(ShaderDiskCachePhaseV_InitializeCreatesDirectory)
{
    ResetDiskCache();
    auto dir = MakeCacheDir("init1");
    std::filesystem::remove_all(dir);
    EXPECT_FALSE(std::filesystem::exists(dir));

    auto& cache = Spark::Graphics::GetShaderDiskCache();
    cache.Initialize(dir);

    EXPECT_TRUE(cache.IsInitialized());
    EXPECT_TRUE(std::filesystem::exists(dir));

    cache.Shutdown();
    std::filesystem::remove_all(dir);
}

TEST(ShaderDiskCachePhaseV_ShutdownClearsInitializedFlag)
{
    ResetDiskCache();
    auto dir = MakeCacheDir("init2");

    auto& cache = Spark::Graphics::GetShaderDiskCache();
    cache.Initialize(dir);
    EXPECT_TRUE(cache.IsInitialized());

    cache.Shutdown();
    EXPECT_FALSE(cache.IsInitialized());

    std::filesystem::remove_all(dir);
}

TEST(ShaderDiskCachePhaseV_InitializeIdempotent)
{
    ResetDiskCache();
    auto dir = MakeCacheDir("init3");

    auto& cache = Spark::Graphics::GetShaderDiskCache();
    cache.Initialize(dir);
    cache.Initialize(dir);

    EXPECT_TRUE(cache.IsInitialized());

    cache.Shutdown();
    std::filesystem::remove_all(dir);
}

// ============================================================================
// Store + Lookup round-trip
// ============================================================================

TEST(ShaderDiskCachePhaseV_StoreLookupRoundTrip)
{
    ResetDiskCache();
    auto dir = MakeCacheDir("rt1");

    auto& cache = Spark::Graphics::GetShaderDiskCache();
    cache.Initialize(dir);

    auto source = MakeSource();
    auto blob = MakeBlob(0xAB, 128);
    cache.Store(source, Spark::Graphics::ShaderTarget::DXBC, blob);

    auto retrieved = cache.Lookup(source, Spark::Graphics::ShaderTarget::DXBC);
    EXPECT_TRUE(retrieved.has_value());
    EXPECT_EQ(retrieved->bytecode.size(), blob.bytecode.size());
    EXPECT_EQ(retrieved->bytecode[0], static_cast<uint8_t>(0xAB));
    EXPECT_EQ(retrieved->bytecode[127], static_cast<uint8_t>(0xAB));
    EXPECT_TRUE(retrieved->success);
    EXPECT_EQ(static_cast<int>(retrieved->stage), static_cast<int>(Spark::Graphics::ShaderStage::Vertex));
    EXPECT_EQ(static_cast<int>(retrieved->target), static_cast<int>(Spark::Graphics::ShaderTarget::DXBC));

    cache.Shutdown();
    std::filesystem::remove_all(dir);
}

// ============================================================================
// Cache key sensitivity — source / defines / stage / target changes
// ============================================================================

TEST(ShaderDiskCachePhaseV_DifferentSourceMisses)
{
    ResetDiskCache();
    auto dir = MakeCacheDir("miss1");

    auto& cache = Spark::Graphics::GetShaderDiskCache();
    cache.Initialize(dir);

    auto a = MakeSource("float4 main():SV_Target{return 0;}");
    auto b = MakeSource("float4 main():SV_Target{return 1;}");

    cache.Store(a, Spark::Graphics::ShaderTarget::DXBC, MakeBlob(0x11));
    auto hitA = cache.Lookup(a, Spark::Graphics::ShaderTarget::DXBC);
    auto missB = cache.Lookup(b, Spark::Graphics::ShaderTarget::DXBC);

    EXPECT_TRUE(hitA.has_value());
    EXPECT_FALSE(missB.has_value());

    cache.Shutdown();
    std::filesystem::remove_all(dir);
}

TEST(ShaderDiskCachePhaseV_DifferentDefinesMisses)
{
    ResetDiskCache();
    auto dir = MakeCacheDir("miss2");

    auto& cache = Spark::Graphics::GetShaderDiskCache();
    cache.Initialize(dir);

    auto a = MakeSource("void main(){}", Spark::Graphics::ShaderStage::Vertex, {"FOO=1"});
    auto b = MakeSource("void main(){}", Spark::Graphics::ShaderStage::Vertex, {"FOO=2"});

    cache.Store(a, Spark::Graphics::ShaderTarget::DXBC, MakeBlob(0x22));
    auto hitA = cache.Lookup(a, Spark::Graphics::ShaderTarget::DXBC);
    auto missB = cache.Lookup(b, Spark::Graphics::ShaderTarget::DXBC);

    EXPECT_TRUE(hitA.has_value());
    EXPECT_FALSE(missB.has_value());

    cache.Shutdown();
    std::filesystem::remove_all(dir);
}

TEST(ShaderDiskCachePhaseV_DifferentStageMisses)
{
    ResetDiskCache();
    auto dir = MakeCacheDir("miss3");

    auto& cache = Spark::Graphics::GetShaderDiskCache();
    cache.Initialize(dir);

    auto vs = MakeSource("void main(){}", Spark::Graphics::ShaderStage::Vertex);
    auto ps = MakeSource("void main(){}", Spark::Graphics::ShaderStage::Pixel);

    cache.Store(vs, Spark::Graphics::ShaderTarget::DXBC, MakeBlob(0x33));
    auto hitVS = cache.Lookup(vs, Spark::Graphics::ShaderTarget::DXBC);
    auto missPS = cache.Lookup(ps, Spark::Graphics::ShaderTarget::DXBC);

    EXPECT_TRUE(hitVS.has_value());
    EXPECT_FALSE(missPS.has_value());

    cache.Shutdown();
    std::filesystem::remove_all(dir);
}

TEST(ShaderDiskCachePhaseV_DifferentTargetMisses)
{
    ResetDiskCache();
    auto dir = MakeCacheDir("miss4");

    auto& cache = Spark::Graphics::GetShaderDiskCache();
    cache.Initialize(dir);

    auto source = MakeSource("void main(){}");
    cache.Store(source, Spark::Graphics::ShaderTarget::DXBC, MakeBlob(0x44));

    auto hitDXBC = cache.Lookup(source, Spark::Graphics::ShaderTarget::DXBC);
    auto missSPIRV = cache.Lookup(source, Spark::Graphics::ShaderTarget::SPIRV);

    EXPECT_TRUE(hitDXBC.has_value());
    EXPECT_FALSE(missSPIRV.has_value());

    cache.Shutdown();
    std::filesystem::remove_all(dir);
}

// ============================================================================
// Lookup on empty / uninitialised cache
// ============================================================================

TEST(ShaderDiskCachePhaseV_LookupOnEmptyReturnsNullopt)
{
    ResetDiskCache();
    auto dir = MakeCacheDir("empty1");

    auto& cache = Spark::Graphics::GetShaderDiskCache();
    cache.Initialize(dir);

    auto result = cache.Lookup(MakeSource(), Spark::Graphics::ShaderTarget::DXBC);
    EXPECT_FALSE(result.has_value());

    cache.Shutdown();
    std::filesystem::remove_all(dir);
}

TEST(ShaderDiskCachePhaseV_LookupOnShutdownReturnsNullopt)
{
    ResetDiskCache();
    auto dir = MakeCacheDir("empty2");

    auto& cache = Spark::Graphics::GetShaderDiskCache();
    cache.Initialize(dir);
    cache.Store(MakeSource(), Spark::Graphics::ShaderTarget::DXBC, MakeBlob(0x55));
    cache.Shutdown();

    // After Shutdown, Lookup should not fire even if the file is still
    // on disk.
    auto result = cache.Lookup(MakeSource(), Spark::Graphics::ShaderTarget::DXBC);
    EXPECT_FALSE(result.has_value());

    std::filesystem::remove_all(dir);
}

// ============================================================================
// Store guards — fails silently on bad inputs
// ============================================================================

TEST(ShaderDiskCachePhaseV_StoreFailedBlobIgnored)
{
    ResetDiskCache();
    auto dir = MakeCacheDir("guard1");

    auto& cache = Spark::Graphics::GetShaderDiskCache();
    cache.Initialize(dir);

    auto source = MakeSource();
    auto failBlob = MakeBlob(0x66);
    failBlob.success = false;
    cache.Store(source, Spark::Graphics::ShaderTarget::DXBC, failBlob);

    auto lookup = cache.Lookup(source, Spark::Graphics::ShaderTarget::DXBC);
    EXPECT_FALSE(lookup.has_value());
    EXPECT_EQ(cache.GetEntryCount(), static_cast<size_t>(0));

    cache.Shutdown();
    std::filesystem::remove_all(dir);
}

TEST(ShaderDiskCachePhaseV_StoreEmptyBytecodeIgnored)
{
    ResetDiskCache();
    auto dir = MakeCacheDir("guard2");

    auto& cache = Spark::Graphics::GetShaderDiskCache();
    cache.Initialize(dir);

    Spark::Graphics::CompiledShaderBlob empty;
    empty.success = true; // success but empty
    cache.Store(MakeSource(), Spark::Graphics::ShaderTarget::DXBC, empty);

    EXPECT_EQ(cache.GetEntryCount(), static_cast<size_t>(0));

    cache.Shutdown();
    std::filesystem::remove_all(dir);
}

// ============================================================================
// Clear / GetEntryCount / GetDiskUsage
// ============================================================================

TEST(ShaderDiskCachePhaseV_EntryCountAndDiskUsage)
{
    ResetDiskCache();
    auto dir = MakeCacheDir("count1");

    auto& cache = Spark::Graphics::GetShaderDiskCache();
    cache.Initialize(dir);

    EXPECT_EQ(cache.GetEntryCount(), static_cast<size_t>(0));
    EXPECT_EQ(cache.GetDiskUsage(), static_cast<size_t>(0));

    cache.Store(MakeSource("a"), Spark::Graphics::ShaderTarget::DXBC, MakeBlob(1, 100));
    cache.Store(MakeSource("b"), Spark::Graphics::ShaderTarget::DXBC, MakeBlob(2, 200));
    cache.Store(MakeSource("c"), Spark::Graphics::ShaderTarget::DXBC, MakeBlob(3, 300));

    EXPECT_EQ(cache.GetEntryCount(), static_cast<size_t>(3));
    EXPECT_EQ(cache.GetDiskUsage(), static_cast<size_t>(600));

    cache.Shutdown();
    std::filesystem::remove_all(dir);
}

TEST(ShaderDiskCachePhaseV_ClearRemovesAllEntries)
{
    ResetDiskCache();
    auto dir = MakeCacheDir("clear1");

    auto& cache = Spark::Graphics::GetShaderDiskCache();
    cache.Initialize(dir);

    cache.Store(MakeSource("a"), Spark::Graphics::ShaderTarget::DXBC, MakeBlob(1));
    cache.Store(MakeSource("b"), Spark::Graphics::ShaderTarget::DXBC, MakeBlob(2));
    EXPECT_EQ(cache.GetEntryCount(), static_cast<size_t>(2));

    cache.Clear();
    EXPECT_EQ(cache.GetEntryCount(), static_cast<size_t>(0));
    EXPECT_TRUE(cache.IsInitialized()); // Clear does not Shutdown

    cache.Shutdown();
    std::filesystem::remove_all(dir);
}

// ============================================================================
// Persistence across Shutdown + Initialize
// ============================================================================

TEST(ShaderDiskCachePhaseV_PersistsAcrossShutdownInitialize)
{
    ResetDiskCache();
    auto dir = MakeCacheDir("persist1");

    auto& cache = Spark::Graphics::GetShaderDiskCache();

    // Store a blob, tear down.
    cache.Initialize(dir);
    auto source = MakeSource("persistent source");
    cache.Store(source, Spark::Graphics::ShaderTarget::DXBC, MakeBlob(0x77, 64));
    cache.Shutdown();

    // Re-initialise against the same directory — the stored blob must
    // still be reachable. This is the invariant that lets engine
    // restarts pick up cached bytecode.
    cache.Initialize(dir);
    auto retrieved = cache.Lookup(source, Spark::Graphics::ShaderTarget::DXBC);
    EXPECT_TRUE(retrieved.has_value());
    EXPECT_EQ(retrieved->bytecode.size(), static_cast<size_t>(64));
    EXPECT_EQ(retrieved->bytecode[0], static_cast<uint8_t>(0x77));

    cache.Shutdown();
    std::filesystem::remove_all(dir);
}
