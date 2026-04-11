/**
 * @file TestShaderCrossCompilerPhaseW.cpp
 * @brief Phase W activation tests for Spark::Graphics::ShaderCrossCompiler
 *
 * These tests exercise behaviours introduced or relied on by the Phase W
 * wire-up into Shader::Initialize:
 *
 *   - GetShaderCrossCompiler() singleton accessor returns a stable
 *     instance (same reference on repeat calls) — required for
 *     cross-TU sharing.
 *   - Initialize / Shutdown toggle IsInitialized correctly.
 *   - Compile on an uninitialised instance returns an empty blob.
 *   - Compile on an initialised instance returns success for each
 *     supported ShaderTarget (DXBC, DXIL, SPIRV, GLSL, MSL).
 *   - Compile caches its result — a second call with the same source
 *     bumps the cache hit counter and the cache size stays stable.
 *   - CompileAll returns at least one blob per target platform.
 *   - ClearCache wipes the in-memory map but keeps IsInitialized().
 *   - CompileAsync returns a future that resolves to the same blob.
 *   - CompileVariantsAsync fans out N compilations and returns N
 *     futures.
 *   - Cache key discriminates on stage (VS vs PS of same source).
 *   - Cache key discriminates on target (same source, DXBC vs SPIRV).
 *   - Cache key discriminates on defines (preprocessor toggle).
 *   - Console_GetStatus returns a non-empty status string.
 *
 * Tests run against the real Spark::Graphics::ShaderCrossCompiler class
 * via the new GetShaderCrossCompiler() accessor — no local reimplemen-
 * tation — per the Phase L / O / P / Q / U / V playbook.
 */

#include "TestFramework.h"
#include "Graphics/ShaderCrossCompiler.h"

#include <future>
#include <string>
#include <vector>

namespace
{

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

    // Reset the singleton between tests so residual cache state from
    // earlier test files cannot affect counters.
    void ResetCrossCompiler()
    {
        auto& xc = Spark::Graphics::GetShaderCrossCompiler();
        xc.Shutdown();
        xc.Initialize();
    }

} // namespace

// ============================================================================
// Singleton accessor stability
// ============================================================================

TEST(ShaderCrossCompilerPhaseW_SingletonReturnsSameInstance)
{
    auto& a = Spark::Graphics::GetShaderCrossCompiler();
    auto& b = Spark::Graphics::GetShaderCrossCompiler();
    EXPECT_TRUE(&a == &b);
}

// ============================================================================
// Initialize / Shutdown
// ============================================================================

TEST(ShaderCrossCompilerPhaseW_InitializeSetsFlag)
{
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();
    xc.Shutdown();
    EXPECT_FALSE(xc.IsInitialized());
    xc.Initialize();
    EXPECT_TRUE(xc.IsInitialized());
}

TEST(ShaderCrossCompilerPhaseW_ShutdownClearsFlagAndCache)
{
    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();

    xc.Compile(MakeSource("float4 main():SV_Target{return 1;}"), Spark::Graphics::ShaderTarget::DXBC);
    EXPECT_TRUE(xc.GetCacheSize() > static_cast<size_t>(0));

    xc.Shutdown();
    EXPECT_FALSE(xc.IsInitialized());
    EXPECT_EQ(xc.GetCacheSize(), static_cast<size_t>(0));

    xc.Initialize();
}

// ============================================================================
// Compile on uninitialised instance
// ============================================================================

TEST(ShaderCrossCompilerPhaseW_CompileBeforeInitializeEmpty)
{
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();
    xc.Shutdown();

    auto blob = xc.Compile(MakeSource(), Spark::Graphics::ShaderTarget::DXBC);
    EXPECT_FALSE(blob.success);
    EXPECT_EQ(blob.bytecode.size(), static_cast<size_t>(0));

    xc.Initialize();
}

// ============================================================================
// Compile for each target
// ============================================================================

TEST(ShaderCrossCompilerPhaseW_CompileDXBCSucceeds)
{
    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();
    auto blob = xc.Compile(MakeSource("float4 main():SV_Target{return 0;}"), Spark::Graphics::ShaderTarget::DXBC);
    EXPECT_TRUE(blob.success);
    EXPECT_EQ(static_cast<int>(blob.target), static_cast<int>(Spark::Graphics::ShaderTarget::DXBC));
}

TEST(ShaderCrossCompilerPhaseW_CompileDXILSucceeds)
{
    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();
    auto blob = xc.Compile(MakeSource("float4 main():SV_Target{return 0;}"), Spark::Graphics::ShaderTarget::DXIL);
    EXPECT_TRUE(blob.success);
    EXPECT_EQ(static_cast<int>(blob.target), static_cast<int>(Spark::Graphics::ShaderTarget::DXIL));
}

TEST(ShaderCrossCompilerPhaseW_CompileSPIRVSucceeds)
{
    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();
    auto blob = xc.Compile(MakeSource("float4 main():SV_Target{return 0;}"), Spark::Graphics::ShaderTarget::SPIRV);
    EXPECT_TRUE(blob.success);
    EXPECT_EQ(static_cast<int>(blob.target), static_cast<int>(Spark::Graphics::ShaderTarget::SPIRV));
}

TEST(ShaderCrossCompilerPhaseW_CompileGLSLSucceeds)
{
    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();
    auto blob = xc.Compile(MakeSource("float4 main():SV_Target{return 0;}"), Spark::Graphics::ShaderTarget::GLSL);
    EXPECT_TRUE(blob.success);
    EXPECT_EQ(static_cast<int>(blob.target), static_cast<int>(Spark::Graphics::ShaderTarget::GLSL));
}

TEST(ShaderCrossCompilerPhaseW_CompileMSLSucceeds)
{
    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();
    auto blob = xc.Compile(MakeSource("float4 main():SV_Target{return 0;}"), Spark::Graphics::ShaderTarget::MSL);
    EXPECT_TRUE(blob.success);
    EXPECT_EQ(static_cast<int>(blob.target), static_cast<int>(Spark::Graphics::ShaderTarget::MSL));
}

// ============================================================================
// Caching behaviour
// ============================================================================

TEST(ShaderCrossCompilerPhaseW_SecondCompileHitsCache)
{
    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();

    auto source = MakeSource("float4 main():SV_Target{return 2;}");
    xc.Compile(source, Spark::Graphics::ShaderTarget::DXBC);
    const auto stats1 = xc.GetCacheStats();
    const size_t size1 = xc.GetCacheSize();

    xc.Compile(source, Spark::Graphics::ShaderTarget::DXBC);
    const auto stats2 = xc.GetCacheStats();
    const size_t size2 = xc.GetCacheSize();

    EXPECT_EQ(size1, size2); // No new entry
    EXPECT_TRUE(stats2.hits > stats1.hits);
}

TEST(ShaderCrossCompilerPhaseW_ClearCacheKeepsInitialized)
{
    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();

    xc.Compile(MakeSource("float4 main():SV_Target{return 3;}"), Spark::Graphics::ShaderTarget::DXBC);
    EXPECT_TRUE(xc.GetCacheSize() > static_cast<size_t>(0));

    xc.ClearCache();
    EXPECT_EQ(xc.GetCacheSize(), static_cast<size_t>(0));
    EXPECT_TRUE(xc.IsInitialized());
}

// ============================================================================
// Cache key discrimination
// ============================================================================

TEST(ShaderCrossCompilerPhaseW_StageDiscriminatesCache)
{
    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();

    xc.Compile(MakeSource("void main(){}", Spark::Graphics::ShaderStage::Vertex), Spark::Graphics::ShaderTarget::DXBC);
    xc.Compile(MakeSource("void main(){}", Spark::Graphics::ShaderStage::Pixel), Spark::Graphics::ShaderTarget::DXBC);

    EXPECT_EQ(xc.GetCacheSize(), static_cast<size_t>(2));
}

TEST(ShaderCrossCompilerPhaseW_TargetDiscriminatesCache)
{
    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();

    xc.Compile(MakeSource("void main(){}"), Spark::Graphics::ShaderTarget::DXBC);
    xc.Compile(MakeSource("void main(){}"), Spark::Graphics::ShaderTarget::SPIRV);

    EXPECT_EQ(xc.GetCacheSize(), static_cast<size_t>(2));
}

TEST(ShaderCrossCompilerPhaseW_DefinesDiscriminateCache)
{
    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();

    xc.Compile(MakeSource("void main(){}", Spark::Graphics::ShaderStage::Vertex, {"USE_FOO=1"}),
               Spark::Graphics::ShaderTarget::DXBC);
    xc.Compile(MakeSource("void main(){}", Spark::Graphics::ShaderStage::Vertex, {"USE_FOO=2"}),
               Spark::Graphics::ShaderTarget::DXBC);

    EXPECT_EQ(xc.GetCacheSize(), static_cast<size_t>(2));
}

// ============================================================================
// CompileAll fan-out
// ============================================================================

TEST(ShaderCrossCompilerPhaseW_CompileAllProducesMultipleTargets)
{
    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();

    auto blobs = xc.CompileAll(MakeSource("void main(){}"));
    EXPECT_TRUE(blobs.size() >= static_cast<size_t>(2));
    // Linux platform produces at least SPIRV + GLSL; Windows adds DXBC.
    for (const auto& blob : blobs)
    {
        EXPECT_TRUE(blob.success);
    }
}

// ============================================================================
// Async compilation
// ============================================================================

TEST(ShaderCrossCompilerPhaseW_CompileAsyncResolvesToBlob)
{
    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();

    auto future = xc.CompileAsync(MakeSource("void main(){}"), Spark::Graphics::ShaderTarget::SPIRV);
    auto blob = future.get();
    EXPECT_TRUE(blob.success);
    EXPECT_EQ(static_cast<int>(blob.target), static_cast<int>(Spark::Graphics::ShaderTarget::SPIRV));
}

TEST(ShaderCrossCompilerPhaseW_CompileVariantsAsyncFanout)
{
    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();

    std::vector<Spark::Graphics::ShaderSource> sources = {
        MakeSource("void main(){}", Spark::Graphics::ShaderStage::Vertex, {"A"}),
        MakeSource("void main(){}", Spark::Graphics::ShaderStage::Vertex, {"B"}),
        MakeSource("void main(){}", Spark::Graphics::ShaderStage::Vertex, {"C"})};
    auto futures = xc.CompileVariantsAsync(sources, Spark::Graphics::ShaderTarget::GLSL);
    EXPECT_EQ(futures.size(), static_cast<size_t>(3));
    for (auto& f : futures)
    {
        auto blob = f.get();
        EXPECT_TRUE(blob.success);
    }
}

// ============================================================================
// Console status
// ============================================================================

TEST(ShaderCrossCompilerPhaseW_ConsoleStatusReturnsString)
{
    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();

    xc.Compile(MakeSource("void main(){}"), Spark::Graphics::ShaderTarget::DXBC);

    const auto status = xc.Console_GetStatus();
    EXPECT_STR_CONTAINS(status, "ShaderCrossCompiler");
    EXPECT_STR_CONTAINS(status, "cached");
}

// ============================================================================
// GetShaderModelForTarget
// ============================================================================

TEST(ShaderCrossCompilerPhaseW_GetShaderModelForTarget)
{
    using Spark::Graphics::ShaderCrossCompiler;
    using Spark::Graphics::ShaderStage;
    using Spark::Graphics::ShaderTarget;

    const std::string dxbcModel = ShaderCrossCompiler::GetShaderModelForTarget(ShaderTarget::DXBC, ShaderStage::Vertex);
    const std::string spirvModel =
        ShaderCrossCompiler::GetShaderModelForTarget(ShaderTarget::SPIRV, ShaderStage::Vertex);

    EXPECT_EQ(dxbcModel, std::string("5_0"));
    EXPECT_EQ(spirvModel, std::string("6_0"));
}
