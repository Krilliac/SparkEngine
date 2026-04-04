// TestAssetMigration.cpp - Tests for Spark::AssetMigrationRegistry and related types
#include "TestFramework.h"
#include "Core/AssetMigration.h"

// ============================================================================
// AssetVersion comparison operators
// ============================================================================

TEST(AssetMigration_Version_Equality)
{
    Spark::AssetVersion a{1, 2, 3};
    Spark::AssetVersion b{1, 2, 3};
    EXPECT_TRUE(a == b);
}

TEST(AssetMigration_Version_Inequality)
{
    Spark::AssetVersion a{1, 2, 3};
    Spark::AssetVersion b{1, 2, 4};
    EXPECT_TRUE(a != b);
}

TEST(AssetMigration_Version_LessThan_Patch)
{
    Spark::AssetVersion a{1, 0, 0};
    Spark::AssetVersion b{1, 0, 1};
    EXPECT_TRUE(a < b);
}

TEST(AssetMigration_Version_LessThan_Minor)
{
    Spark::AssetVersion a{1, 0, 9};
    Spark::AssetVersion b{1, 1, 0};
    EXPECT_TRUE(a < b);
}

TEST(AssetMigration_Version_LessThan_Major)
{
    Spark::AssetVersion a{1, 9, 9};
    Spark::AssetVersion b{2, 0, 0};
    EXPECT_TRUE(a < b);
}

TEST(AssetMigration_Version_NotLessThan_Equal)
{
    Spark::AssetVersion a{1, 0, 0};
    Spark::AssetVersion b{1, 0, 0};
    EXPECT_FALSE(a < b);
}

TEST(AssetMigration_Version_ToString)
{
    Spark::AssetVersion v{2, 5, 1};
    EXPECT_TRUE(v.ToString() == "2.5.1");
}

// ============================================================================
// AssetFileHeader magic value
// ============================================================================

TEST(AssetMigration_Header_MagicIsSPRK)
{
    Spark::AssetFileHeader header;
    EXPECT_EQ(header.magic, 0x5350524Bu);
}

TEST(AssetMigration_Header_DefaultVersion)
{
    Spark::AssetFileHeader header;
    EXPECT_EQ(header.version.major, static_cast<uint16_t>(1));
    EXPECT_EQ(header.version.minor, static_cast<uint16_t>(0));
    EXPECT_EQ(header.version.patch, static_cast<uint16_t>(0));
}

// ============================================================================
// ValidateHeader
// ============================================================================

TEST(AssetMigration_ValidateHeader_ValidDefault)
{
    Spark::AssetFileHeader header;
    header.headerSize = 32; // Non-zero for validation
    EXPECT_TRUE(Spark::ValidateHeader(header));
}

TEST(AssetMigration_ValidateHeader_BadMagic)
{
    Spark::AssetFileHeader header;
    header.headerSize = 32;
    header.magic = 0xDEADBEEF;
    EXPECT_FALSE(Spark::ValidateHeader(header));
}

TEST(AssetMigration_ValidateHeader_ZeroHeaderSize)
{
    Spark::AssetFileHeader header;
    header.headerSize = 0;
    EXPECT_FALSE(Spark::ValidateHeader(header));
}

// ============================================================================
// NeedsMigration
// ============================================================================

TEST(AssetMigration_NeedsMigration_MatchingVersion_ReturnsFalse)
{
    auto& registry = Spark::AssetMigrationRegistry::GetInstance();
    registry.Initialize();

    Spark::AssetFileHeader header;
    header.headerSize = 32;
    header.version = {1, 0, 0};
    header.assetType = Spark::AssetType::Scene;

    // Default current version is 1.0.0, so no migration needed
    EXPECT_FALSE(registry.NeedsMigration(header));

    registry.Shutdown();
}

TEST(AssetMigration_NeedsMigration_OlderVersion_ReturnsTrue)
{
    auto& registry = Spark::AssetMigrationRegistry::GetInstance();
    registry.Initialize();
    registry.SetCurrentVersion(Spark::AssetType::Scene, {2, 0, 0});

    Spark::AssetFileHeader header;
    header.headerSize = 32;
    header.version = {1, 0, 0};
    header.assetType = Spark::AssetType::Scene;

    EXPECT_TRUE(registry.NeedsMigration(header));

    registry.Shutdown();
}

// ============================================================================
// RegisterMigration and GetMigrationPath
// ============================================================================

/// Stub migration step for testing.
class StubMigrationStep final : public Spark::IMigrationStep
{
  public:
    StubMigrationStep(Spark::AssetVersion from, Spark::AssetVersion to) : m_from(from), m_to(to) {}

    Spark::AssetVersion GetSourceVersion() const override { return m_from; }
    Spark::AssetVersion GetTargetVersion() const override { return m_to; }
    bool Migrate(Spark::BinaryReader& /*reader*/, Spark::BinaryWriter& /*writer*/) override { return true; }
    std::string_view GetDescription() const override { return "stub"; }

  private:
    Spark::AssetVersion m_from;
    Spark::AssetVersion m_to;
};

TEST(AssetMigration_RegisterMigration_GetMigrationPath)
{
    auto& registry = Spark::AssetMigrationRegistry::GetInstance();
    registry.Initialize();

    registry.RegisterMigration(
        std::make_unique<StubMigrationStep>(Spark::AssetVersion{1, 0, 0}, Spark::AssetVersion{2, 0, 0}));

    auto path = registry.GetMigrationPath({1, 0, 0}, {2, 0, 0}, Spark::AssetType::Scene);
    EXPECT_EQ(path.size(), 1u);

    registry.Shutdown();
}

TEST(AssetMigration_GetMigrationPath_NoPath)
{
    auto& registry = Spark::AssetMigrationRegistry::GetInstance();
    registry.Initialize();

    // No steps registered, so no path from 1.0.0 to 3.0.0
    auto path = registry.GetMigrationPath({1, 0, 0}, {3, 0, 0}, Spark::AssetType::Scene);
    EXPECT_TRUE(path.empty());

    registry.Shutdown();
}

TEST(AssetMigration_GetMigrationPath_MultiStep)
{
    auto& registry = Spark::AssetMigrationRegistry::GetInstance();
    registry.Initialize();

    registry.RegisterMigration(
        std::make_unique<StubMigrationStep>(Spark::AssetVersion{1, 0, 0}, Spark::AssetVersion{2, 0, 0}));
    registry.RegisterMigration(
        std::make_unique<StubMigrationStep>(Spark::AssetVersion{2, 0, 0}, Spark::AssetVersion{3, 0, 0}));

    auto path = registry.GetMigrationPath({1, 0, 0}, {3, 0, 0}, Spark::AssetType::Scene);
    EXPECT_EQ(path.size(), 2u);

    registry.Shutdown();
}

// ============================================================================
// GetCurrentVersion
// ============================================================================

TEST(AssetMigration_GetCurrentVersion_DefaultIs100)
{
    auto& registry = Spark::AssetMigrationRegistry::GetInstance();
    registry.Initialize();

    auto ver = registry.GetCurrentVersion(Spark::AssetType::Scene);
    EXPECT_EQ(ver.major, static_cast<uint16_t>(1));
    EXPECT_EQ(ver.minor, static_cast<uint16_t>(0));
    EXPECT_EQ(ver.patch, static_cast<uint16_t>(0));

    registry.Shutdown();
}

TEST(AssetMigration_GetCurrentVersion_AfterSetCurrentVersion)
{
    auto& registry = Spark::AssetMigrationRegistry::GetInstance();
    registry.Initialize();

    registry.SetCurrentVersion(Spark::AssetType::Material, {3, 1, 0});
    auto ver = registry.GetCurrentVersion(Spark::AssetType::Material);
    EXPECT_EQ(ver.major, static_cast<uint16_t>(3));
    EXPECT_EQ(ver.minor, static_cast<uint16_t>(1));
    EXPECT_EQ(ver.patch, static_cast<uint16_t>(0));

    registry.Shutdown();
}

// ============================================================================
// CRC32
// ============================================================================

TEST(AssetMigration_ComputeCRC32_NonZero)
{
    const uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    uint32_t crc = Spark::ComputeCRC32(data, sizeof(data));
    EXPECT_NE(crc, 0u);
}

TEST(AssetMigration_ComputeCRC32_Deterministic)
{
    const uint8_t data[] = {0xAA, 0xBB, 0xCC};
    uint32_t crc1 = Spark::ComputeCRC32(data, sizeof(data));
    uint32_t crc2 = Spark::ComputeCRC32(data, sizeof(data));
    EXPECT_EQ(crc1, crc2);
}
