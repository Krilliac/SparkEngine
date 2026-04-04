// TestAssetValidator.cpp - Tests for Spark::AssetValidator
#include "TestFramework.h"
#include "Core/AssetValidator.h"

// ============================================================================
// Initialize / Shutdown lifecycle
// ============================================================================

TEST(AssetValidator_Initialize_Shutdown)
{
    auto& validator = Spark::AssetValidator::GetInstance();
    validator.Initialize();

    // After init, built-in rules should be registered
    auto rules = validator.GetRegisteredRules();
    EXPECT_GT(rules.size(), 0u);

    validator.Shutdown();

    // After shutdown, rules should be cleared
    auto rulesAfter = validator.GetRegisteredRules();
    EXPECT_EQ(rulesAfter.size(), 0u);
}

// ============================================================================
// RegisterRule
// ============================================================================

/// Simple test rule that does nothing but has a name.
class TestValidationRule final : public Spark::IAssetValidationRule
{
  public:
    void ValidateAsset(const std::filesystem::path& /*path*/, Spark::ValidationReport& /*report*/) override {}
    std::string_view GetRuleName() const override { return "TestRule"; }
};

TEST(AssetValidator_RegisterRule_AddsRule)
{
    auto& validator = Spark::AssetValidator::GetInstance();
    validator.Initialize();

    size_t countBefore = validator.GetRegisteredRules().size();
    validator.RegisterRule(std::make_unique<TestValidationRule>());
    size_t countAfter = validator.GetRegisteredRules().size();

    EXPECT_EQ(countAfter, countBefore + 1);

    validator.Shutdown();
}

TEST(AssetValidator_RegisterRule_NullIgnored)
{
    auto& validator = Spark::AssetValidator::GetInstance();
    validator.Initialize();

    size_t countBefore = validator.GetRegisteredRules().size();
    validator.RegisterRule(nullptr);
    size_t countAfter = validator.GetRegisteredRules().size();

    EXPECT_EQ(countAfter, countBefore);

    validator.Shutdown();
}

// ============================================================================
// GetRegisteredRules
// ============================================================================

TEST(AssetValidator_GetRegisteredRules_ContainsBuiltins)
{
    auto& validator = Spark::AssetValidator::GetInstance();
    validator.Initialize();

    auto rules = validator.GetRegisteredRules();

    // Should have the 4 built-in rules
    EXPECT_EQ(rules.size(), 4u);

    bool hasMaterial = false;
    bool hasScene = false;
    bool hasShader = false;
    bool hasMetadata = false;

    for (auto name : rules)
    {
        if (name == "MaterialTextureValidator")
            hasMaterial = true;
        if (name == "SceneReferenceValidator")
            hasScene = true;
        if (name == "ShaderCompilationValidator")
            hasShader = true;
        if (name == "AssetMetadataValidator")
            hasMetadata = true;
    }

    EXPECT_TRUE(hasMaterial);
    EXPECT_TRUE(hasScene);
    EXPECT_TRUE(hasShader);
    EXPECT_TRUE(hasMetadata);

    validator.Shutdown();
}

// ============================================================================
// ValidateAll
// ============================================================================

TEST(AssetValidator_ValidateAll_ReturnsReport)
{
    auto& validator = Spark::AssetValidator::GetInstance();
    validator.Initialize();

    // ValidateAll scans "Assets" directory which may not exist in test env.
    // It should still return a valid report (possibly with an error about missing dir).
    auto report = validator.ValidateAll();

    // The report object should be valid regardless
    EXPECT_GE(report.durationMs, 0.0f);
    EXPECT_FALSE(report.timestamp.empty());

    validator.Shutdown();
}

// ============================================================================
// Console_GetStatus
// ============================================================================

TEST(AssetValidator_Console_GetStatus_NotInitialized)
{
    auto& validator = Spark::AssetValidator::GetInstance();
    validator.Shutdown();

    auto status = validator.Console_GetStatus();
    EXPECT_FALSE(status.empty());
    EXPECT_STR_CONTAINS(status, "not initialized");
}

TEST(AssetValidator_Console_GetStatus_Initialized)
{
    auto& validator = Spark::AssetValidator::GetInstance();
    validator.Initialize();

    auto status = validator.Console_GetStatus();
    EXPECT_FALSE(status.empty());
    EXPECT_STR_CONTAINS(status, "initialized");
    EXPECT_STR_CONTAINS(status, "rule");

    validator.Shutdown();
}
