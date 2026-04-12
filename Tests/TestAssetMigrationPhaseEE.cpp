/**
 * @file TestAssetMigrationPhaseEE.cpp
 * @brief Phase EE Theme 3D tests for Spark::AssetMigrationRegistry
 *
 * AssetMigrationRegistry is a header-only asset-versioning registry.
 * Phase EE wires Initialize / Shutdown into the engine lifecycle.
 */

#include "TestFramework.h"
#include "Core/AssetMigration.h"

TEST(AssetMigrationPhaseEE_SingletonStable)
{
    auto& a = Spark::AssetMigrationRegistry::GetInstance();
    auto& b = Spark::AssetMigrationRegistry::GetInstance();
    EXPECT_TRUE(&a == &b);
}

TEST(AssetMigrationPhaseEE_InitializeShutdown)
{
    auto& reg = Spark::AssetMigrationRegistry::GetInstance();
    reg.Shutdown();
    reg.Initialize();
    reg.Shutdown();
    reg.Initialize();
    // Leave initialised.
}

TEST(AssetMigrationPhaseEE_DefaultVersionsAfterInit)
{
    auto& reg = Spark::AssetMigrationRegistry::GetInstance();
    reg.Initialize();
    // After Initialize, every AssetType has a default version of 1.0.0.
    const auto v = reg.GetCurrentVersion(Spark::AssetType::Scene);
    EXPECT_EQ(v.major, static_cast<uint16_t>(1));
    EXPECT_EQ(v.minor, static_cast<uint16_t>(0));
    EXPECT_EQ(v.patch, static_cast<uint16_t>(0));
}

TEST(AssetMigrationPhaseEE_SetCurrentVersionOverride)
{
    auto& reg = Spark::AssetMigrationRegistry::GetInstance();
    reg.Initialize();

    Spark::AssetVersion v;
    v.major = 2;
    v.minor = 3;
    v.patch = 4;
    reg.SetCurrentVersion(Spark::AssetType::Material, v);

    const auto retrieved = reg.GetCurrentVersion(Spark::AssetType::Material);
    EXPECT_EQ(retrieved.major, static_cast<uint16_t>(2));
    EXPECT_EQ(retrieved.minor, static_cast<uint16_t>(3));
    EXPECT_EQ(retrieved.patch, static_cast<uint16_t>(4));

    reg.Initialize(); // Reset for other tests.
}

TEST(AssetMigrationPhaseEE_NeedsMigrationOnOlderHeader)
{
    auto& reg = Spark::AssetMigrationRegistry::GetInstance();
    reg.Initialize();

    // Bump the current version of Scene to 2.0.0.
    Spark::AssetVersion current;
    current.major = 2;
    current.minor = 0;
    current.patch = 0;
    reg.SetCurrentVersion(Spark::AssetType::Scene, current);

    // Header with version 1.0.0 should be detected as needing migration.
    Spark::AssetFileHeader header;
    header.version = {1, 0, 0};
    header.assetType = Spark::AssetType::Scene;
    header.headerSize = sizeof(Spark::AssetFileHeader);
    EXPECT_TRUE(reg.NeedsMigration(header));

    reg.Initialize(); // Reset.
}

TEST(AssetMigrationPhaseEE_NeedsMigrationFalseForCurrentHeader)
{
    auto& reg = Spark::AssetMigrationRegistry::GetInstance();
    reg.Initialize();

    Spark::AssetFileHeader header;
    header.version = {1, 0, 0}; // Matches default current version.
    header.assetType = Spark::AssetType::Prefab;
    header.headerSize = sizeof(Spark::AssetFileHeader);
    EXPECT_FALSE(reg.NeedsMigration(header));
}

TEST(AssetMigrationPhaseEE_ConsoleStatusAfterInit)
{
    auto& reg = Spark::AssetMigrationRegistry::GetInstance();
    reg.Initialize();
    const std::string status = reg.Console_GetStatus();
    EXPECT_TRUE(!status.empty());
    // Should contain "initialized".
    EXPECT_TRUE(status.find("initialized") != std::string::npos);
}

TEST(AssetMigrationPhaseEE_ConsoleStatusWhenUninitialized)
{
    auto& reg = Spark::AssetMigrationRegistry::GetInstance();
    reg.Shutdown();
    const std::string status = reg.Console_GetStatus();
    EXPECT_TRUE(!status.empty());
    EXPECT_TRUE(status.find("not initialized") != std::string::npos);
    reg.Initialize(); // Restore.
}

TEST(AssetMigrationPhaseEE_AssetVersionComparison)
{
    Spark::AssetVersion v1{1, 0, 0};
    Spark::AssetVersion v2{1, 0, 1};
    Spark::AssetVersion v3{2, 0, 0};

    EXPECT_TRUE(v1 < v2);
    EXPECT_TRUE(v2 < v3);
    EXPECT_TRUE(v1 < v3);
    EXPECT_TRUE(v1 == v1);
    EXPECT_TRUE(v3 > v1);
}

TEST(AssetMigrationPhaseEE_AssetVersionToString)
{
    Spark::AssetVersion v{1, 2, 3};
    const std::string s = v.ToString();
    EXPECT_EQ(s, std::string("1.2.3"));
}
