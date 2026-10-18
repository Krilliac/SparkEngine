// TestAssetMigration.cpp - Tests for Spark::AssetMigrationRegistry and related types
#include "TestFramework.h"
#include "Core/AssetMigration.h"

#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

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

// The memset below zeroes the magic too; it is set again so that ValidateHeader passes and
// the type and version checks are what these two tests exercise (each has a control case).
TEST(AssetMigration_MigrateAsset_RejectsExpectedTypeMismatch)
{
    Spark::AssetFileHeader header{};
    std::memset(&header, 0, sizeof(header));
    header.magic = 0x5350524B;
    header.version = {1, 0, 0};
    header.assetType = Spark::AssetType::Scene;
    header.headerSize = sizeof(Spark::AssetFileHeader);
    header.dataSize = 0;
    header.checksum = 0;

    std::vector<uint8_t> data(sizeof(Spark::AssetFileHeader));
    std::memcpy(data.data(), &header, sizeof(header));
    std::vector<uint8_t> control = data;

    auto& registry = Spark::AssetMigrationRegistry::GetInstance();
    registry.Initialize();

    EXPECT_FALSE(registry.MigrateAsset(data, Spark::AssetType::Material));
    EXPECT_TRUE(registry.MigrateAsset(control, Spark::AssetType::Scene)); // already current

    registry.Shutdown();
}

TEST(AssetMigration_MigrateAsset_RejectsFutureVersionWithoutMutation)
{
    Spark::AssetFileHeader header{};
    std::memset(&header, 0, sizeof(header));
    header.magic = 0x5350524B;
    header.assetType = Spark::AssetType::Scene;
    header.version = {2, 0, 0};
    header.headerSize = sizeof(Spark::AssetFileHeader);
    header.dataSize = 0;
    header.checksum = 0;

    std::vector<uint8_t> data(sizeof(Spark::AssetFileHeader));
    std::memcpy(data.data(), &header, sizeof(header));
    const std::vector<uint8_t> original = data;

    header.version = {1, 0, 0};
    std::vector<uint8_t> control(sizeof(Spark::AssetFileHeader));
    std::memcpy(control.data(), &header, sizeof(header));

    auto& registry = Spark::AssetMigrationRegistry::GetInstance();
    registry.Initialize();

    EXPECT_FALSE(registry.MigrateAsset(data, Spark::AssetType::Scene));
    EXPECT_TRUE(data == original);
    EXPECT_TRUE(registry.MigrateAsset(control, Spark::AssetType::Scene)); // already current

    registry.Shutdown();
}

// ============================================================================
// SEC-120 asset-migration target: a migrated file's header must describe it
// ============================================================================

namespace
{
    /// Copies the payload and appends one marker byte, so a test can see it ran.
    class AppendMarkerStep final : public Spark::IMigrationStep
    {
      public:
        AppendMarkerStep(Spark::AssetVersion from, Spark::AssetVersion to) : m_from(from), m_to(to) {}

        Spark::AssetVersion GetSourceVersion() const override { return m_from; }
        Spark::AssetVersion GetTargetVersion() const override { return m_to; }
        bool Migrate(Spark::BinaryReader& reader, Spark::BinaryWriter& writer) override
        {
            std::vector<uint8_t> bytes(reader.Remaining());
            if (!reader.ReadBytes(bytes.data(), bytes.size()))
            {
                return false;
            }
            writer.WriteBytes(bytes.data(), bytes.size());
            writer.Write<uint8_t>(0xA5);
            return true;
        }
        std::string_view GetDescription() const override { return "append marker"; }

      private:
        Spark::AssetVersion m_from;
        Spark::AssetVersion m_to;
    };

    constexpr uint32_t kFixedHeaderBytes = sizeof(Spark::AssetFileHeader);

    /// A version 1.0.0 Scene file whose header (padded with 0xEE up to @p headerSize)
    /// describes @p payload.
    std::vector<uint8_t> MakeSceneFile(const std::vector<uint8_t>& payload, uint32_t headerSize = kFixedHeaderBytes)
    {
        Spark::AssetFileHeader header{};
        std::memset(&header, 0, sizeof(header));
        header.magic = 0x5350524B;
        header.version = {1, 0, 0};
        header.assetType = Spark::AssetType::Scene;
        header.headerSize = headerSize;
        header.dataSize = payload.size();
        header.checksum = payload.empty() ? 0 : Spark::ComputeCRC32(payload.data(), payload.size());

        std::vector<uint8_t> file(headerSize, 0xEE);
        std::memcpy(file.data(), &header, sizeof(header));
        file.insert(file.end(), payload.begin(), payload.end());
        return file;
    }

    Spark::AssetFileHeader HeaderOf(const std::vector<uint8_t>& file)
    {
        Spark::AssetFileHeader header{};
        std::memcpy(&header, file.data(), sizeof(header));
        return header;
    }

    void RegisterSceneMigrationToV2(Spark::AssetMigrationRegistry& registry)
    {
        registry.Initialize();
        registry.RegisterMigration(
            std::make_unique<AppendMarkerStep>(Spark::AssetVersion{1, 0, 0}, Spark::AssetVersion{2, 0, 0}));
        registry.SetCurrentVersion(Spark::AssetType::Scene, {2, 0, 0});
    }
} // namespace

TEST(AssetMigration_ValidateHeader_RejectsHeaderSizeBelowFixedFields)
{
    Spark::AssetFileHeader header;
    header.headerSize = 8; // the payload would start inside the header's own fields
    EXPECT_FALSE(Spark::ValidateHeader(header));
    header.headerSize = kFixedHeaderBytes;
    EXPECT_TRUE(Spark::ValidateHeader(header));
}

TEST(AssetMigration_MigrateAsset_MigratesVerifiedPayload)
{
    auto& registry = Spark::AssetMigrationRegistry::GetInstance();
    RegisterSceneMigrationToV2(registry);

    std::vector<uint8_t> file = MakeSceneFile({1, 2, 3});
    ASSERT_TRUE(registry.MigrateAsset(file, Spark::AssetType::Scene));

    const std::vector<uint8_t> expectedPayload = {1, 2, 3, 0xA5};
    const Spark::AssetFileHeader header = HeaderOf(file);
    EXPECT_TRUE(header.version == (Spark::AssetVersion{2, 0, 0}));
    EXPECT_EQ(header.headerSize, kFixedHeaderBytes);
    EXPECT_EQ(header.dataSize, static_cast<uint64_t>(expectedPayload.size()));
    EXPECT_EQ(header.checksum, Spark::ComputeCRC32(expectedPayload.data(), expectedPayload.size()));
    EXPECT_TRUE(std::vector<uint8_t>(file.begin() + kFixedHeaderBytes, file.end()) == expectedPayload);

    registry.Shutdown();
}

TEST(AssetMigration_MigrateAsset_RefusesPayloadThatFailsItsChecksum)
{
    auto& registry = Spark::AssetMigrationRegistry::GetInstance();
    RegisterSceneMigrationToV2(registry);

    std::vector<uint8_t> file = MakeSceneFile({1, 2, 3});
    file.back() ^= 0x40; // corrupt the payload after the checksum was computed
    const std::vector<uint8_t> original = file;

    // Migrating would stamp a fresh checksum over the corrupt payload.
    EXPECT_FALSE(registry.MigrateAsset(file, Spark::AssetType::Scene));
    EXPECT_TRUE(file == original);

    registry.Shutdown();
}

TEST(AssetMigration_MigrateAsset_RefusesPayloadOfWrongDeclaredSize)
{
    auto& registry = Spark::AssetMigrationRegistry::GetInstance();
    RegisterSceneMigrationToV2(registry);

    std::vector<uint8_t> file = MakeSceneFile({1, 2, 3, 4});
    file.pop_back(); // truncated: dataSize still says four bytes
    Spark::AssetFileHeader header = HeaderOf(file);
    const std::vector<uint8_t> kept(file.begin() + kFixedHeaderBytes, file.end());
    header.checksum = Spark::ComputeCRC32(kept.data(), kept.size()); // only the size is wrong
    std::memcpy(file.data(), &header, sizeof(header));
    const std::vector<uint8_t> original = file;

    EXPECT_FALSE(registry.MigrateAsset(file, Spark::AssetType::Scene));
    EXPECT_TRUE(file == original);

    registry.Shutdown();
}

TEST(AssetMigration_MigrateAsset_RewritesLongerHeaderSize)
{
    auto& registry = Spark::AssetMigrationRegistry::GetInstance();
    RegisterSceneMigrationToV2(registry);

    // A newer writer's 48-byte header: 16 extension bytes before the payload.
    std::vector<uint8_t> file = MakeSceneFile({7, 8, 9}, kFixedHeaderBytes + 16);
    ASSERT_TRUE(registry.MigrateAsset(file, Spark::AssetType::Scene));

    // The extension bytes are not carried over, so the payload follows the fixed fields and
    // the header has to say so; a reader that honours headerSize must find the payload.
    const Spark::AssetFileHeader header = HeaderOf(file);
    EXPECT_EQ(header.headerSize, kFixedHeaderBytes);
    ASSERT_TRUE(file.size() >= header.headerSize);
    const std::vector<uint8_t> payload(file.begin() + header.headerSize, file.end());
    EXPECT_TRUE(payload == (std::vector<uint8_t>{7, 8, 9, 0xA5}));

    registry.Shutdown();
}

TEST(AssetMigration_GetMigrationPath_IgnoresStepThatDoesNotAdvance)
{
    auto& registry = Spark::AssetMigrationRegistry::GetInstance();
    registry.Initialize();
    // A step whose target equals its source used to be picked forever: the search never
    // advanced and the path vector grew without bound.
    registry.RegisterMigration(
        std::make_unique<AppendMarkerStep>(Spark::AssetVersion{1, 0, 0}, Spark::AssetVersion{1, 0, 0}));

    EXPECT_TRUE(registry.GetMigrationPath({1, 0, 0}, {2, 0, 0}, Spark::AssetType::Scene).empty());

    registry.Shutdown();
}
