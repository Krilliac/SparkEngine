/**
 * @file TestShaderVariantSystem.cpp
 * @brief Phase O — CPU reference tests for ShaderVariantSystem
 *
 * Exercises the header-only `Spark::Graphics::ShaderVariantSystem`
 * directly — no GPU, no D3D11, no compiled shaders. The variant
 * system is pure CPU keyword / variant bookkeeping so every test
 * runs on every platform's CI.
 *
 * Phase O also wires the system as a per-instance member of the
 * `Shader` class with Initialize/Shutdown hooks on both the Windows
 * and Linux code paths.
 */

#include "TestFramework.h"

#include "Graphics/ShaderVariantSystem.h"

#include <unordered_set>

using Spark::Graphics::KeywordGroup;
using Spark::Graphics::KeywordType;
using Spark::Graphics::ShaderKeyword;
using Spark::Graphics::ShaderVariantKey;
using Spark::Graphics::ShaderVariantSystem;

namespace
{
    ShaderKeyword MakeKeyword(const std::string& name, KeywordType type = KeywordType::MultiCompile,
                              bool defaultEnabled = false)
    {
        ShaderKeyword kw;
        kw.name = name;
        kw.defineSymbol = name;
        kw.type = type;
        kw.defaultEnabled = defaultEnabled;
        return kw;
    }
} // namespace

// =========================================================================
// Lifecycle
// =========================================================================

TEST(ShaderVariantSystem_InitializeAndShutdown)
{
    ShaderVariantSystem svs;
    EXPECT_FALSE(svs.IsInitialized());
    svs.Initialize();
    EXPECT_TRUE(svs.IsInitialized());
    EXPECT_EQ(svs.GetKeywordCount(), 0);
    svs.Shutdown();
    EXPECT_FALSE(svs.IsInitialized());
}

TEST(ShaderVariantSystem_ReinitializeClearsState)
{
    ShaderVariantSystem svs;
    svs.Initialize();
    svs.RegisterKeyword(MakeKeyword("_NORMALMAP"));
    svs.RegisterKeyword(MakeKeyword("_PARALLAX"));
    EXPECT_EQ(svs.GetKeywordCount(), 2);

    // Re-initialising clears keywords + resets the next-index counter.
    svs.Initialize();
    EXPECT_EQ(svs.GetKeywordCount(), 0);
    EXPECT_EQ(svs.GetGlobalVariantKey().bits, 0u);
    svs.Shutdown();
}

// =========================================================================
// Keyword registration
// =========================================================================

TEST(ShaderVariantSystem_RegisterKeywordAssignsSequentialIndices)
{
    ShaderVariantSystem svs;
    svs.Initialize();
    int a = svs.RegisterKeyword(MakeKeyword("_A"));
    int b = svs.RegisterKeyword(MakeKeyword("_B"));
    int c = svs.RegisterKeyword(MakeKeyword("_C"));
    EXPECT_EQ(a, 0);
    EXPECT_EQ(b, 1);
    EXPECT_EQ(c, 2);
    EXPECT_EQ(svs.GetKeywordCount(), 3);
    svs.Shutdown();
}

TEST(ShaderVariantSystem_RegisterKeywordWithDefaultEnabled)
{
    ShaderVariantSystem svs;
    svs.Initialize();
    svs.RegisterKeyword(MakeKeyword("_NORMALMAP", KeywordType::MultiCompile, /*defaultEnabled*/ true));
    EXPECT_TRUE(svs.IsGlobalKeywordEnabled("_NORMALMAP"));
    // The global state should reflect bit 0 set.
    EXPECT_EQ(svs.GetGlobalVariantKey().bits, 1u);
    svs.Shutdown();
}

TEST(ShaderVariantSystem_GetKeywordByIndex)
{
    ShaderVariantSystem svs;
    svs.Initialize();
    svs.RegisterKeyword(MakeKeyword("_NORMALMAP"));
    const auto* kw = svs.GetKeyword(0);
    ASSERT_TRUE(kw != nullptr);
    EXPECT_EQ(kw->name, std::string("_NORMALMAP"));
    // Out-of-range indices return null.
    EXPECT_TRUE(svs.GetKeyword(-1) == nullptr);
    EXPECT_TRUE(svs.GetKeyword(999) == nullptr);
    svs.Shutdown();
}

TEST(ShaderVariantSystem_GetKeywordIndexUnknownIsNegativeOne)
{
    ShaderVariantSystem svs;
    svs.Initialize();
    EXPECT_EQ(svs.GetKeywordIndex("_MISSING"), -1);
    svs.RegisterKeyword(MakeKeyword("_A"));
    EXPECT_EQ(svs.GetKeywordIndex("_A"), 0);
    EXPECT_EQ(svs.GetKeywordIndex("_MISSING"), -1);
    svs.Shutdown();
}

TEST(ShaderVariantSystem_RegisterKeywordGroupAllocatesContiguous)
{
    ShaderVariantSystem svs;
    svs.Initialize();
    KeywordGroup group;
    group.groupName = "Shadows";
    group.keywords = {"_SHADOWS_OFF", "_SHADOWS_LOW", "_SHADOWS_HIGH"};
    group.defaultIndex = 1;
    group.type = KeywordType::MultiCompile;

    int firstIndex = svs.RegisterKeywordGroup(group);
    EXPECT_EQ(firstIndex, 0);
    EXPECT_EQ(svs.GetKeywordCount(), 3);
    EXPECT_EQ(svs.GetKeywordIndex("_SHADOWS_OFF"), 0);
    EXPECT_EQ(svs.GetKeywordIndex("_SHADOWS_LOW"), 1);
    EXPECT_EQ(svs.GetKeywordIndex("_SHADOWS_HIGH"), 2);

    // Default index → _SHADOWS_LOW should be enabled.
    EXPECT_TRUE(svs.IsGlobalKeywordEnabled("_SHADOWS_LOW"));
    EXPECT_FALSE(svs.IsGlobalKeywordEnabled("_SHADOWS_OFF"));
    EXPECT_FALSE(svs.IsGlobalKeywordEnabled("_SHADOWS_HIGH"));
    svs.Shutdown();
}

// =========================================================================
// Keyword state management
// =========================================================================

TEST(ShaderVariantSystem_SetGlobalKeywordToggles)
{
    ShaderVariantSystem svs;
    svs.Initialize();
    svs.RegisterKeyword(MakeKeyword("_FOG"));
    EXPECT_FALSE(svs.IsGlobalKeywordEnabled("_FOG"));
    svs.SetGlobalKeyword("_FOG", true);
    EXPECT_TRUE(svs.IsGlobalKeywordEnabled("_FOG"));
    svs.SetGlobalKeyword("_FOG", false);
    EXPECT_FALSE(svs.IsGlobalKeywordEnabled("_FOG"));
    svs.Shutdown();
}

TEST(ShaderVariantSystem_SetGlobalKeywordUnknownIsNoop)
{
    ShaderVariantSystem svs;
    svs.Initialize();
    svs.SetGlobalKeyword("_MISSING", true); // no crash
    EXPECT_EQ(svs.GetGlobalVariantKey().bits, 0u);
    svs.Shutdown();
}

TEST(ShaderVariantSystem_GetGlobalVariantKeyReflectsBits)
{
    ShaderVariantSystem svs;
    svs.Initialize();
    svs.RegisterKeyword(MakeKeyword("_A"));
    svs.RegisterKeyword(MakeKeyword("_B"));
    svs.RegisterKeyword(MakeKeyword("_C"));
    svs.SetGlobalKeyword("_A", true);
    svs.SetGlobalKeyword("_C", true);
    EXPECT_EQ(svs.GetGlobalVariantKey().bits, 0b101u);
    svs.Shutdown();
}

// =========================================================================
// Variant key operations
// =========================================================================

TEST(ShaderVariantSystem_VariantKeyEquality)
{
    ShaderVariantKey a{0b1010};
    ShaderVariantKey b{0b1010};
    ShaderVariantKey c{0b0101};
    EXPECT_TRUE(a == b);
    EXPECT_TRUE(a != c);
    EXPECT_TRUE(c < a);
}

TEST(ShaderVariantSystem_VariantKeyHasKeyword)
{
    ShaderVariantKey k{0b1010};
    EXPECT_FALSE(k.HasKeyword(0));
    EXPECT_TRUE(k.HasKeyword(1));
    EXPECT_FALSE(k.HasKeyword(2));
    EXPECT_TRUE(k.HasKeyword(3));
}

TEST(ShaderVariantSystem_VariantKeySetKeyword)
{
    ShaderVariantKey k;
    k.SetKeyword(5, true);
    EXPECT_TRUE(k.HasKeyword(5));
    EXPECT_EQ(k.bits, 1ULL << 5);
    k.SetKeyword(5, false);
    EXPECT_FALSE(k.HasKeyword(5));
    EXPECT_EQ(k.bits, 0u);
}

// =========================================================================
// Variant resolution
// =========================================================================

TEST(ShaderVariantSystem_ResolveVariantKeyOrsGlobalAndMaterial)
{
    ShaderVariantSystem svs;
    svs.Initialize();
    svs.RegisterKeyword(MakeKeyword("_A"));
    svs.RegisterKeyword(MakeKeyword("_B"));
    svs.RegisterKeyword(MakeKeyword("_C"));
    svs.SetGlobalKeyword("_A", true); // bit 0

    uint64_t materialBits = (1ULL << 2); // _C set at the material level
    ShaderVariantKey resolved = svs.ResolveVariantKey(materialBits);
    EXPECT_EQ(resolved.bits, 0b101u); // A + C
    svs.Shutdown();
}

TEST(ShaderVariantSystem_GetDefinesForVariantReturnsEnabled)
{
    ShaderVariantSystem svs;
    svs.Initialize();
    svs.RegisterKeyword(MakeKeyword("_NORMALMAP"));
    svs.RegisterKeyword(MakeKeyword("_PARALLAX"));
    svs.RegisterKeyword(MakeKeyword("_FOG"));

    ShaderVariantKey key;
    key.SetKeyword(0, true);
    key.SetKeyword(2, true);

    auto defines = svs.GetDefinesForVariant(key);
    EXPECT_EQ(static_cast<int>(defines.size()), 2);
    EXPECT_EQ(defines[0], std::string("_NORMALMAP"));
    EXPECT_EQ(defines[1], std::string("_FOG"));
    svs.Shutdown();
}

// =========================================================================
// Variant counting + stripping
// =========================================================================

TEST(ShaderVariantSystem_GetTotalVariantCount)
{
    ShaderVariantSystem svs;
    svs.Initialize();
    // 0 keywords → 1 variant (the default).
    EXPECT_EQ(svs.GetTotalVariantCount({}), 1u);
    // N keywords → 2^N variants.
    EXPECT_EQ(svs.GetTotalVariantCount({0}), 2u);
    EXPECT_EQ(svs.GetTotalVariantCount({0, 1}), 4u);
    EXPECT_EQ(svs.GetTotalVariantCount({0, 1, 2}), 8u);
    EXPECT_EQ(svs.GetTotalVariantCount({0, 1, 2, 3}), 16u);
    svs.Shutdown();
}

TEST(ShaderVariantSystem_StripUnusedShaderFeatures)
{
    ShaderVariantSystem svs;
    svs.Initialize();
    svs.RegisterKeyword(MakeKeyword("_MULTI_A", KeywordType::MultiCompile));
    svs.RegisterKeyword(MakeKeyword("_FEATURE_B", KeywordType::ShaderFeature));
    svs.RegisterKeyword(MakeKeyword("_FEATURE_C", KeywordType::ShaderFeature));
    svs.RegisterKeyword(MakeKeyword("_FEATURE_D", KeywordType::ShaderFeature));

    std::unordered_set<std::string> usedKeywords = {"_FEATURE_B"};
    uint32_t stripped = svs.StripUnusedVariants(usedKeywords);
    // _FEATURE_C and _FEATURE_D are ShaderFeatures not in the used set
    // → 2 stripped. _MULTI_A is MultiCompile so never stripped.
    EXPECT_EQ(stripped, 2u);
    svs.Shutdown();
}

TEST(ShaderVariantSystem_ShouldCompileVariantSkipsDisabledFeatures)
{
    ShaderVariantSystem svs;
    svs.Initialize();
    svs.RegisterKeyword(MakeKeyword("_DXR_ENABLED"));
    svs.RegisterKeyword(MakeKeyword("_NORMALMAP"));

    std::unordered_set<std::string> disabled = {"_DXR_ENABLED"};

    // Variant with _DXR_ENABLED must be rejected.
    ShaderVariantKey dxrOn;
    dxrOn.SetKeyword(0, true);
    EXPECT_FALSE(svs.ShouldCompileVariant(dxrOn, disabled));

    // Variant without _DXR_ENABLED must pass even if _NORMALMAP is on.
    ShaderVariantKey normalOnly;
    normalOnly.SetKeyword(1, true);
    EXPECT_TRUE(svs.ShouldCompileVariant(normalOnly, disabled));
    svs.Shutdown();
}

// =========================================================================
// Pre-warming
// =========================================================================

TEST(ShaderVariantSystem_GetWarmupVariantsIncludesDefaultAndToggles)
{
    ShaderVariantSystem svs;
    svs.Initialize();
    svs.RegisterKeyword(MakeKeyword("_A", KeywordType::MultiCompile));
    svs.RegisterKeyword(MakeKeyword("_B", KeywordType::MultiCompile));

    std::unordered_set<std::string> used;
    auto variants = svs.GetWarmupVariants(used);
    // Default variant + one per MultiCompile keyword toggled → 3 entries.
    EXPECT_EQ(static_cast<int>(variants.size()), 3);
    svs.Shutdown();
}

// =========================================================================
// Console status
// =========================================================================

TEST(ShaderVariantSystem_Console_GetStatusFormatting)
{
    ShaderVariantSystem svs;
    svs.Initialize();
    svs.RegisterKeyword(MakeKeyword("_NORMALMAP", KeywordType::MultiCompile, true));
    svs.RegisterKeyword(MakeKeyword("_FEATURE", KeywordType::ShaderFeature));
    std::string status = svs.Console_GetStatus();
    EXPECT_TRUE(status.find("ShaderVariantSystem") != std::string::npos);
    EXPECT_TRUE(status.find("_NORMALMAP") != std::string::npos);
    EXPECT_TRUE(status.find("_FEATURE") != std::string::npos);
    // Default-enabled keyword prints as ON.
    EXPECT_TRUE(status.find("ON") != std::string::npos);
    EXPECT_TRUE(status.find("OFF") != std::string::npos);
    svs.Shutdown();
}

// =========================================================================
// kMaxKeywords limit
// =========================================================================

TEST(ShaderVariantSystem_ExceedingKMaxKeywordsReturnsNegative)
{
    ShaderVariantSystem svs;
    svs.Initialize();
    // Register exactly kMaxKeywords — all should succeed.
    for (int i = 0; i < ShaderVariantSystem::kMaxKeywords; ++i)
    {
        int index = svs.RegisterKeyword(MakeKeyword("_K" + std::to_string(i)));
        EXPECT_EQ(index, i);
    }
    // The next one must fail.
    int overflow = svs.RegisterKeyword(MakeKeyword("_OVERFLOW"));
    EXPECT_EQ(overflow, -1);
    EXPECT_EQ(svs.GetKeywordCount(), ShaderVariantSystem::kMaxKeywords);
    svs.Shutdown();
}
