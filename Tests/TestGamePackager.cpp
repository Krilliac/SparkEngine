// TestGamePackager.cpp - Tests for Spark::GamePackager
#include "TestFramework.h"
#include "Core/GamePackager.h"

// ============================================================================
// PackageConfig validation
// ============================================================================

TEST(GamePackager_ValidateConfig_DefaultConfig_Valid)
{
    auto& packager = Spark::GamePackager::GetInstance();
    packager.Initialize();

    Spark::PackageConfig cfg;
    auto errors = packager.ValidateConfig(cfg);
    EXPECT_TRUE(errors.empty());

    packager.Shutdown();
}

TEST(GamePackager_ValidateConfig_EmptyOutputDir)
{
    auto& packager = Spark::GamePackager::GetInstance();
    packager.Initialize();

    Spark::PackageConfig cfg;
    cfg.outputDir = "";
    auto errors = packager.ValidateConfig(cfg);
    EXPECT_FALSE(errors.empty());

    bool foundError = false;
    for (const auto& e : errors)
    {
        if (e.find("Output directory") != std::string::npos)
            foundError = true;
    }
    EXPECT_TRUE(foundError);

    packager.Shutdown();
}

TEST(GamePackager_ValidateConfig_EmptyProjectName)
{
    auto& packager = Spark::GamePackager::GetInstance();
    packager.Initialize();

    Spark::PackageConfig cfg;
    cfg.projectName = "";
    auto errors = packager.ValidateConfig(cfg);
    EXPECT_FALSE(errors.empty());

    packager.Shutdown();
}

TEST(GamePackager_ValidateConfig_InvalidProjectNameChars)
{
    auto& packager = Spark::GamePackager::GetInstance();
    packager.Initialize();

    Spark::PackageConfig cfg;
    cfg.projectName = "My:Game";
    auto errors = packager.ValidateConfig(cfg);
    EXPECT_FALSE(errors.empty());

    bool foundCharError = false;
    for (const auto& e : errors)
    {
        if (e.find("invalid filesystem characters") != std::string::npos)
            foundCharError = true;
    }
    EXPECT_TRUE(foundCharError);

    packager.Shutdown();
}

TEST(GamePackager_ValidateConfig_NotInitialized)
{
    auto& packager = Spark::GamePackager::GetInstance();
    packager.Shutdown(); // Ensure not initialized

    Spark::PackageConfig cfg;
    auto errors = packager.ValidateConfig(cfg);
    EXPECT_FALSE(errors.empty());

    bool foundInitError = false;
    for (const auto& e : errors)
    {
        if (e.find("not been initialized") != std::string::npos)
            foundInitError = true;
    }
    EXPECT_TRUE(foundInitError);
}

// ============================================================================
// GetSupportedPlatforms
// ============================================================================

TEST(GamePackager_GetSupportedPlatforms_AfterInit_NonEmpty)
{
    auto& packager = Spark::GamePackager::GetInstance();
    packager.Initialize();

    auto platforms = packager.GetSupportedPlatforms();
    EXPECT_FALSE(platforms.empty());

    packager.Shutdown();
}

TEST(GamePackager_GetSupportedPlatforms_AfterShutdown_Empty)
{
    auto& packager = Spark::GamePackager::GetInstance();
    packager.Initialize();
    packager.Shutdown();

    auto platforms = packager.GetSupportedPlatforms();
    EXPECT_TRUE(platforms.empty());
}

// ============================================================================
// Package with invalid config
// ============================================================================

TEST(GamePackager_Package_InvalidConfig_ReturnsErrors)
{
    auto& packager = Spark::GamePackager::GetInstance();
    packager.Initialize();

    Spark::PackageConfig cfg;
    cfg.projectName = ""; // Invalid
    auto result = packager.Package(cfg);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errors.empty());

    packager.Shutdown();
}

// ============================================================================
// Console_GetStatus
// ============================================================================

TEST(GamePackager_Console_GetStatus_NotInitialized)
{
    auto& packager = Spark::GamePackager::GetInstance();
    packager.Shutdown();

    auto status = packager.Console_GetStatus();
    EXPECT_FALSE(status.empty());
    EXPECT_STR_CONTAINS(status, "not initialized");
}

TEST(GamePackager_Console_GetStatus_Initialized)
{
    auto& packager = Spark::GamePackager::GetInstance();
    packager.Initialize();

    auto status = packager.Console_GetStatus();
    EXPECT_FALSE(status.empty());
    EXPECT_STR_CONTAINS(status, "initialized");
    EXPECT_STR_CONTAINS(status, "supported platform");

    packager.Shutdown();
}
