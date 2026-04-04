/**
 * @file TestFBXImporter.cpp
 * @brief Tests for FBX binary format importer
 */

#include "TestFramework.h"
#include "Graphics/FBXImporter.h"

#include <cstring>

using namespace Spark::Graphics;

TEST(FBXImporter_CanImportDetectsMagicBytes)
{
    auto& importer = FBXImporter::GetInstance();

    // Valid FBX magic: "Kaydara FBX Binary  " (21 bytes) + 0x00 + 0x1a + version
    const char magic[] = "Kaydara FBX Binary  \x00\x1a\x00";
    uint32_t version = 7400;
    std::vector<uint8_t> validData(27);
    std::memcpy(validData.data(), magic, 23);
    std::memcpy(validData.data() + 23, &version, 4);

    EXPECT_TRUE(importer.CanImportFromMemory(validData.data(), validData.size()));

    // Invalid magic
    std::vector<uint8_t> invalidData = {0x89, 'P', 'N', 'G', 0};
    EXPECT_FALSE(importer.CanImportFromMemory(invalidData.data(), invalidData.size()));
}

TEST(FBXImporter_RejectsNullData)
{
    auto& importer = FBXImporter::GetInstance();
    EXPECT_FALSE(importer.CanImportFromMemory(nullptr, 0));
    EXPECT_FALSE(importer.CanImportFromMemory(nullptr, 100));
}

TEST(FBXImporter_RejectsTooSmallData)
{
    auto& importer = FBXImporter::GetInstance();
    uint8_t tiny[5] = {0};
    EXPECT_FALSE(importer.CanImportFromMemory(tiny, 5));
}

TEST(FBXImporter_ImportFromMemoryValidHeader)
{
    auto& importer = FBXImporter::GetInstance();

    const char magic[] = "Kaydara FBX Binary  \x00\x1a\x00";
    uint32_t version = 7400;
    std::vector<uint8_t> data(64, 0);
    std::memcpy(data.data(), magic, 23);
    std::memcpy(data.data() + 23, &version, 4);

    auto result = importer.ImportFromMemory(data.data(), data.size());
    EXPECT_TRUE(result.success);
    // Version info should appear in warnings
    EXPECT_TRUE(!result.warnings.empty());
}

TEST(FBXImporter_SupportedExtensions)
{
    auto& importer = FBXImporter::GetInstance();
    auto exts = importer.GetSupportedExtensions();
    EXPECT_EQ(exts.size(), static_cast<size_t>(1));
    EXPECT_EQ(exts[0], ".fbx");
}

TEST(FBXImporter_InvalidFileReturnsFailure)
{
    auto& importer = FBXImporter::GetInstance();
    auto result = importer.Import("nonexistent_file.fbx");
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(!result.warnings.empty());
}

TEST(FBXImporter_ImportOptionsDefaults)
{
    FBXImportOptions opts;
    EXPECT_NEAR(opts.scaleFactor, 1.0f, 0.001f);
    EXPECT_TRUE(opts.importNormals);
    EXPECT_TRUE(opts.importUVs);
    EXPECT_TRUE(opts.importAnimations);
    EXPECT_TRUE(opts.triangulate);
    EXPECT_FALSE(opts.flipUVs);
}

TEST(FBXImporter_EmptyResultOnInvalidData)
{
    auto& importer = FBXImporter::GetInstance();
    std::vector<uint8_t> garbage(100, 0x42);
    auto result = importer.ImportFromMemory(garbage.data(), garbage.size());
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.meshes.size(), static_cast<size_t>(0));
}
