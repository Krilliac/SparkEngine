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
 *   - DXBC is a real compile: valid HLSL yields a blob whose first four
 *     bytes are 'DXBC', and broken HLSL fails with a compiler diagnostic.
 *   - DXIL / SPIR-V / GLSL / MSL have no compiler integrated and report
 *     `success = false` with the missing dependency named — they used to
 *     report success with zero bytecode, and these tests used to assert
 *     that success, which locked the stub in.
 *   - Only successful compilations are cached; a failed target adds no
 *     cache entry.
 *   - Compile caches its result — a second call with the same source
 *     bumps the cache hit counter and the cache size stays stable.
 *   - ClearCache wipes the in-memory map but keeps IsInitialized().
 *   - CompileAsync / CompileVariantsAsync resolve to the same blobs.
 *   - Cache key discriminates on stage and on defines.
 *   - Console_GetStatus returns a non-empty status string.
 *
 * Tests run against the real Spark::Graphics::ShaderCrossCompiler class
 * via the GetShaderCrossCompiler() accessor — no local reimplementation.
 * The DXBC path needs d3dcompiler_47, so tests that require a successful
 * compilation skip on non-Windows rather than assert a different outcome.
 */

#include "TestFramework.h"
#include "Graphics/ShaderCrossCompiler.h"

#include <future>
#include <string>
#include <vector>

namespace
{

    // Valid vs_5_0 / ps_5_0 bodies. A vertex shader must write SV_Position and
    // a pixel shader must write SV_Target, so the two stages cannot share one
    // source once the compiler is real.
    const char* const kVertexSource = "struct VSOutput { float4 position : SV_Position; };\n"
                                      "VSOutput main(float3 position : POSITION)\n"
                                      "{ VSOutput o; o.position = float4(position, 1.0f); return o; }\n";

    const char* const kPixelSource = "float4 main() : SV_Target { return float4(1, 0, 0, 1); }\n";

    Spark::Graphics::ShaderSource MakeSource(const std::string& hlsl = kVertexSource,
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

#ifdef _WIN32
    constexpr bool kDXBCCompilerAvailable = true;
#else
    constexpr bool kDXBCCompilerAvailable = false;
#endif

    bool StartsWithDXBC(const std::vector<uint8_t>& bytecode)
    {
        return bytecode.size() >= 4 && bytecode[0] == 'D' && bytecode[1] == 'X' && bytecode[2] == 'B' &&
               bytecode[3] == 'C';
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
    if constexpr (!kDXBCCompilerAvailable)
        SKIP_TEST("Populating the cache needs the DXBC compiler (Windows only)");

    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();

    xc.Compile(MakeSource(), Spark::Graphics::ShaderTarget::DXBC);
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
// DXBC is a real compile
// ============================================================================

TEST(ShaderCrossCompilerPhaseW_CompileDXBCProducesBytecode)
{
    if constexpr (!kDXBCCompilerAvailable)
        SKIP_TEST("DXBC compilation needs d3dcompiler_47 (Windows only)");

    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();
    auto blob = xc.Compile(MakeSource(), Spark::Graphics::ShaderTarget::DXBC);

    EXPECT_TRUE(blob.success);
    EXPECT_EQ(static_cast<int>(blob.target), static_cast<int>(Spark::Graphics::ShaderTarget::DXBC));
    EXPECT_TRUE(StartsWithDXBC(blob.bytecode));
    EXPECT_TRUE(blob.errors.empty());
}

TEST(ShaderCrossCompilerPhaseW_CompileDXBCRejectsBrokenHLSL)
{
    if constexpr (!kDXBCCompilerAvailable)
        SKIP_TEST("DXBC compilation needs d3dcompiler_47 (Windows only)");

    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();
    auto blob = xc.Compile(MakeSource("float4 main() : SV_Target { return this_is_not_hlsl(; }"),
                           Spark::Graphics::ShaderTarget::DXBC);

    EXPECT_FALSE(blob.success);
    EXPECT_EQ(blob.bytecode.size(), static_cast<size_t>(0));
    EXPECT_FALSE(blob.errors.empty());
    EXPECT_EQ(xc.GetCacheSize(), static_cast<size_t>(0)); // failures are never cached
}

// ============================================================================
// Targets with no compiler behind them fail closed
// ============================================================================

TEST(ShaderCrossCompilerPhaseW_CompileDXILReportsNotImplemented)
{
    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();
    auto blob = xc.Compile(MakeSource(), Spark::Graphics::ShaderTarget::DXIL);

    EXPECT_FALSE(blob.success);
    EXPECT_EQ(blob.bytecode.size(), static_cast<size_t>(0));
    EXPECT_STR_CONTAINS(blob.errors, "not implemented");
    EXPECT_STR_CONTAINS(blob.errors, "DXC");
}

TEST(ShaderCrossCompilerPhaseW_CompileSPIRVReportsNotImplemented)
{
    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();
    auto blob = xc.Compile(MakeSource(), Spark::Graphics::ShaderTarget::SPIRV);

    EXPECT_FALSE(blob.success);
    EXPECT_EQ(blob.bytecode.size(), static_cast<size_t>(0));
    EXPECT_STR_CONTAINS(blob.errors, "DXC");
}

TEST(ShaderCrossCompilerPhaseW_CompileGLSLReportsNotImplemented)
{
    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();
    auto blob = xc.Compile(MakeSource(), Spark::Graphics::ShaderTarget::GLSL);

    EXPECT_FALSE(blob.success);
    EXPECT_STR_CONTAINS(blob.errors, "SPIRV-Cross");
}

TEST(ShaderCrossCompilerPhaseW_CompileMSLReportsNotImplemented)
{
    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();
    auto blob = xc.Compile(MakeSource(), Spark::Graphics::ShaderTarget::MSL);

    EXPECT_FALSE(blob.success);
    EXPECT_STR_CONTAINS(blob.errors, "SPIRV-Cross");
}

TEST(ShaderCrossCompilerPhaseW_UnimplementedTargetsAreNotCached)
{
    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();

    xc.Compile(MakeSource(), Spark::Graphics::ShaderTarget::SPIRV);
    xc.Compile(MakeSource(), Spark::Graphics::ShaderTarget::GLSL);
    xc.Compile(MakeSource(), Spark::Graphics::ShaderTarget::MSL);

    EXPECT_EQ(xc.GetCacheSize(), static_cast<size_t>(0));
}

// ============================================================================
// Caching behaviour
// ============================================================================

TEST(ShaderCrossCompilerPhaseW_SecondCompileHitsCache)
{
    if constexpr (!kDXBCCompilerAvailable)
        SKIP_TEST("Populating the cache needs the DXBC compiler (Windows only)");

    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();

    auto source = MakeSource();
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
    if constexpr (!kDXBCCompilerAvailable)
        SKIP_TEST("Populating the cache needs the DXBC compiler (Windows only)");

    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();

    xc.Compile(MakeSource(), Spark::Graphics::ShaderTarget::DXBC);
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
    if constexpr (!kDXBCCompilerAvailable)
        SKIP_TEST("Populating the cache needs the DXBC compiler (Windows only)");

    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();

    const auto vs = xc.Compile(MakeSource(kVertexSource, Spark::Graphics::ShaderStage::Vertex),
                               Spark::Graphics::ShaderTarget::DXBC);
    const auto ps =
        xc.Compile(MakeSource(kPixelSource, Spark::Graphics::ShaderStage::Pixel), Spark::Graphics::ShaderTarget::DXBC);

    EXPECT_TRUE(vs.success);
    EXPECT_TRUE(ps.success);
    EXPECT_NE(static_cast<int>(vs.stage), static_cast<int>(ps.stage));
    EXPECT_NE(vs.bytecode.size(), static_cast<size_t>(0));
    EXPECT_EQ(xc.GetCacheSize(), static_cast<size_t>(2));
}

TEST(ShaderCrossCompilerPhaseW_TargetDiscriminatesCache)
{
    if constexpr (!kDXBCCompilerAvailable)
        SKIP_TEST("Populating the cache needs the DXBC compiler (Windows only)");

    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();

    xc.Compile(MakeSource(), Spark::Graphics::ShaderTarget::DXBC);
    xc.Compile(MakeSource(), Spark::Graphics::ShaderTarget::SPIRV);

    // Only DXBC compiles, so the SPIR-V attempt must leave the cache alone
    // instead of inserting a zero-byte "success" entry.
    EXPECT_EQ(xc.GetCacheSize(), static_cast<size_t>(1));
}

TEST(ShaderCrossCompilerPhaseW_DefinesDiscriminateCache)
{
    if constexpr (!kDXBCCompilerAvailable)
        SKIP_TEST("Populating the cache needs the DXBC compiler (Windows only)");

    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();

    xc.Compile(MakeSource(kVertexSource, Spark::Graphics::ShaderStage::Vertex, {"USE_FOO=1"}),
               Spark::Graphics::ShaderTarget::DXBC);
    xc.Compile(MakeSource(kVertexSource, Spark::Graphics::ShaderStage::Vertex, {"USE_FOO=2"}),
               Spark::Graphics::ShaderTarget::DXBC);

    EXPECT_EQ(xc.GetCacheSize(), static_cast<size_t>(2));
}

// ============================================================================
// CompileAll fan-out
// ============================================================================

TEST(ShaderCrossCompilerPhaseW_CompileAllReportsPerTargetTruth)
{
    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();

    auto blobs = xc.CompileAll(MakeSource());
    EXPECT_TRUE(blobs.size() >= static_cast<size_t>(2));

    for (const auto& blob : blobs)
    {
        if (blob.target == Spark::Graphics::ShaderTarget::DXBC)
        {
            EXPECT_EQ(blob.success, kDXBCCompilerAvailable);
        }
        else
        {
            // SPIR-V and GLSL have no compiler: never a silent success.
            EXPECT_FALSE(blob.success);
            EXPECT_FALSE(blob.errors.empty());
        }
    }
}

// ============================================================================
// Async compilation
// ============================================================================

TEST(ShaderCrossCompilerPhaseW_CompileAsyncResolvesToBlob)
{
    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();

    auto future = xc.CompileAsync(MakeSource(), Spark::Graphics::ShaderTarget::DXBC);
    auto blob = future.get();
    EXPECT_EQ(static_cast<int>(blob.target), static_cast<int>(Spark::Graphics::ShaderTarget::DXBC));
    EXPECT_EQ(blob.success, kDXBCCompilerAvailable);
}

TEST(ShaderCrossCompilerPhaseW_CompileVariantsAsyncFanout)
{
    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();

    std::vector<Spark::Graphics::ShaderSource> sources = {
        MakeSource(kVertexSource, Spark::Graphics::ShaderStage::Vertex, {"A"}),
        MakeSource(kVertexSource, Spark::Graphics::ShaderStage::Vertex, {"B"}),
        MakeSource(kVertexSource, Spark::Graphics::ShaderStage::Vertex, {"C"})};
    auto futures = xc.CompileVariantsAsync(sources, Spark::Graphics::ShaderTarget::DXBC);
    EXPECT_EQ(futures.size(), static_cast<size_t>(3));
    for (auto& f : futures)
    {
        auto blob = f.get();
        EXPECT_EQ(blob.success, kDXBCCompilerAvailable);
    }
}

// ============================================================================
// Console status
// ============================================================================

TEST(ShaderCrossCompilerPhaseW_ConsoleStatusReturnsString)
{
    ResetCrossCompiler();
    auto& xc = Spark::Graphics::GetShaderCrossCompiler();

    xc.Compile(MakeSource(), Spark::Graphics::ShaderTarget::DXBC);

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
