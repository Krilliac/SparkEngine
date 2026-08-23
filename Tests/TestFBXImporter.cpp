/**
 * @file TestFBXImporter.cpp
 * @brief Tests for FBX binary format importer
 */

#include "TestFramework.h"
#include "Graphics/FBXImporter.h"

#include <cstring>
#include <limits>

#if defined(SPARK_MINIZ_AVAILABLE) && SPARK_MINIZ_AVAILABLE
#include <miniz.h>
#endif

using namespace Spark::Graphics;

namespace
{
    template <typename T> void WriteAt(std::vector<uint8_t>& bytes, size_t offset, T value)
    {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }

    std::vector<uint8_t> MakeFBX(uint32_t version, size_t size)
    {
        const char magic[] = "Kaydara FBX Binary  \x00\x1a\x00";
        std::vector<uint8_t> bytes(size, 0);
        std::memcpy(bytes.data(), magic, 23);
        WriteAt(bytes, 23, version);
        return bytes;
    }

#if defined(SPARK_MINIZ_AVAILABLE) && SPARK_MINIZ_AVAILABLE
    std::vector<uint8_t> CompressBytes(const void* source, size_t sourceSize)
    {
        mz_ulong compressedSize = mz_compressBound(static_cast<mz_ulong>(sourceSize));
        std::vector<uint8_t> compressed(compressedSize);
        const int status = mz_compress2(compressed.data(), &compressedSize, static_cast<const unsigned char*>(source),
                                        static_cast<mz_ulong>(sourceSize), MZ_BEST_COMPRESSION);
        if (status != MZ_OK)
            return {};
        compressed.resize(static_cast<size_t>(compressedSize));
        return compressed;
    }

    std::vector<uint8_t> MakeCompressedIntArrayFBX(const std::vector<uint8_t>& compressed, uint32_t count)
    {
        constexpr size_t kNodeHeaderOffset = 27;
        constexpr size_t kPropertyOffset = 41;
        constexpr size_t kArrayHeaderBytes = 13;
        constexpr size_t kRecordBytes = 13;
        const size_t propertyBytes = kArrayHeaderBytes + compressed.size();
        const size_t nodeEnd = kPropertyOffset + propertyBytes + kRecordBytes;
        auto bytes = MakeFBX(7400, nodeEnd + kRecordBytes);

        WriteAt<uint32_t>(bytes, kNodeHeaderOffset, static_cast<uint32_t>(nodeEnd));
        WriteAt<uint32_t>(bytes, kNodeHeaderOffset + 4, 1);
        WriteAt<uint32_t>(bytes, kNodeHeaderOffset + 8, static_cast<uint32_t>(propertyBytes));
        bytes[kNodeHeaderOffset + 12] = 1;
        bytes[kNodeHeaderOffset + 13] = 'X';
        bytes[kPropertyOffset] = static_cast<uint8_t>(FBXConstants::PROP_INT32_ARRAY);
        WriteAt<uint32_t>(bytes, kPropertyOffset + 1, count);
        WriteAt<uint32_t>(bytes, kPropertyOffset + 5, 1);
        WriteAt<uint32_t>(bytes, kPropertyOffset + 9, static_cast<uint32_t>(compressed.size()));
        std::memcpy(bytes.data() + kPropertyOffset + kArrayHeaderBytes, compressed.data(), compressed.size());
        return bytes;
    }
#endif
} // namespace

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

    auto invalidTrailer = validData;
    invalidTrailer[21] ^= 0x01;
    EXPECT_FALSE(importer.CanImportFromMemory(invalidTrailer.data(), invalidTrailer.size()));
    invalidTrailer = validData;
    invalidTrailer[22] ^= 0x01;
    EXPECT_FALSE(importer.CanImportFromMemory(invalidTrailer.data(), invalidTrailer.size()));

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
    std::vector<uint8_t> data(27, 0);
    std::memcpy(data.data(), magic, 23);
    std::memcpy(data.data() + 23, &version, 4);

    auto result = importer.ImportFromMemory(data.data(), data.size());
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, std::string("FBX contains no document nodes"));
}

TEST(FBXImporter_ImportsStructurallyValidMinimalNodeTree)
{
    auto& importer = FBXImporter::GetInstance();
    const char magic[] = "Kaydara FBX Binary  \x00\x1a\x00";
    const uint32_t version = 7400;
    std::vector<uint8_t> data(67, 0);
    std::memcpy(data.data(), magic, 23);
    std::memcpy(data.data() + 23, &version, 4);

    const uint32_t endOffset = 54;
    std::memcpy(data.data() + 27, &endOffset, 4);
    data[39] = 1; // name length after end/property-count/property-list-length
    data[40] = 'X';

    const auto result = importer.ImportFromMemory(data.data(), data.size());
    EXPECT_EQ(result.errorMessage, std::string(""));
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.fbxVersion, 7400u);
}

TEST(FBXImporter_ImportsStructurallyValid7500NodeTree)
{
    auto data = MakeFBX(7500, 103);
    WriteAt<uint64_t>(data, 27, 78);
    data[51] = 1;
    data[52] = 'X';

    const auto result = FBXImporter::GetInstance().ImportFromMemory(data.data(), data.size());
    EXPECT_EQ(result.errorMessage, std::string(""));
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.fbxVersion, 7500u);
}

TEST(FBXImporter_RejectsChildHeaderCrossingParentBoundary)
{
    auto v7400 = MakeFBX(7400, 61);
    WriteAt<uint32_t>(v7400, 27, 48);
    v7400[39] = 1;
    v7400[40] = 'X';
    EXPECT_FALSE(FBXImporter::GetInstance().ImportFromMemory(v7400.data(), v7400.size()).success);

    auto v7500 = MakeFBX(7500, 90);
    WriteAt<uint64_t>(v7500, 27, 65);
    v7500[51] = 1;
    v7500[52] = 'X';
    EXPECT_FALSE(FBXImporter::GetInstance().ImportFromMemory(v7500.data(), v7500.size()).success);
}

TEST(FBXImporter_RejectsMalformedNullAndEmptyNodeRecords)
{
    auto nonzeroNull = MakeFBX(7400, 67);
    WriteAt<uint32_t>(nonzeroNull, 27, 54);
    nonzeroNull[39] = 1;
    nonzeroNull[40] = 'X';
    WriteAt<uint32_t>(nonzeroNull, 45, 1); // nonzero property count in the child null record
    EXPECT_FALSE(FBXImporter::GetInstance().ImportFromMemory(nonzeroNull.data(), nonzeroNull.size()).success);

    auto emptyNamedNode = MakeFBX(7400, 53);
    WriteAt<uint32_t>(emptyNamedNode, 27, 53);
    EXPECT_FALSE(FBXImporter::GetInstance().ImportFromMemory(emptyNamedNode.data(), emptyNamedNode.size()).success);

    auto childConsumesParentSentinel = MakeFBX(7400, 93);
    WriteAt<uint32_t>(childConsumesParentSentinel, 27, 80);
    childConsumesParentSentinel[39] = 1;
    childConsumesParentSentinel[40] = 'P';
    WriteAt<uint32_t>(childConsumesParentSentinel, 41, 80);
    childConsumesParentSentinel[53] = 1;
    childConsumesParentSentinel[54] = 'C';
    EXPECT_FALSE(FBXImporter::GetInstance()
                     .ImportFromMemory(childConsumesParentSentinel.data(), childConsumesParentSentinel.size())
                     .success);
}

TEST(FBXImporter_RejectsEveryTruncated7400Sentinel)
{
    for (size_t fileSize = 41; fileSize < 54; ++fileSize)
    {
        auto data = MakeFBX(7400, fileSize);
        WriteAt<uint32_t>(data, 27, static_cast<uint32_t>(fileSize));
        data[39] = 1;
        data[40] = 'X';
        EXPECT_FALSE(FBXImporter::GetInstance().ImportFromMemory(data.data(), data.size()).success);
    }
}

TEST(FBXImporter_RejectsInvalidArrayEncodingBeforeAllocation)
{
    auto data = MakeFBX(7400, 80);
    WriteAt<uint32_t>(data, 27, 67);
    WriteAt<uint32_t>(data, 31, 1);  // property count
    WriteAt<uint32_t>(data, 35, 13); // property byte length
    data[39] = 1;
    data[40] = 'X';
    data[41] = static_cast<uint8_t>(FBXConstants::PROP_DOUBLE_ARRAY);
    WriteAt<uint32_t>(data, 42, 1024u * 1024u);
    WriteAt<uint32_t>(data, 46, 99); // unsupported encoding
    WriteAt<uint32_t>(data, 50, 0);

    const auto result = FBXImporter::GetInstance().ImportFromMemory(data.data(), data.size());
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, std::string("Malformed FBX node tree"));
}

#if defined(SPARK_MINIZ_AVAILABLE) && SPARK_MINIZ_AVAILABLE
TEST(FBXImporter_ValidatesCompressedArrayStreamExactly)
{
    const int32_t values[] = {1, -2, 3, 4000};
    const auto compressed = CompressBytes(values, sizeof(values));
    EXPECT_TRUE(!compressed.empty());

    auto valid = MakeCompressedIntArrayFBX(compressed, 4);
    EXPECT_TRUE(FBXImporter::GetInstance().ImportFromMemory(valid.data(), valid.size()).success);

    auto truncatedBytes = compressed;
    truncatedBytes.pop_back();
    auto truncated = MakeCompressedIntArrayFBX(truncatedBytes, 4);
    EXPECT_FALSE(FBXImporter::GetInstance().ImportFromMemory(truncated.data(), truncated.size()).success);

    auto decodedLengthMismatch = MakeCompressedIntArrayFBX(compressed, 5);
    EXPECT_FALSE(FBXImporter::GetInstance()
                     .ImportFromMemory(decodedLengthMismatch.data(), decodedLengthMismatch.size())
                     .success);

    auto trailingBytes = compressed;
    trailingBytes.push_back(0x5a);
    auto trailing = MakeCompressedIntArrayFBX(trailingBytes, 4);
    EXPECT_FALSE(FBXImporter::GetInstance().ImportFromMemory(trailing.data(), trailing.size()).success);
}
#endif

TEST(FBXImporter_RejectsOverflowingNodeEndOffset)
{
    auto& importer = FBXImporter::GetInstance();
    const char magic[] = "Kaydara FBX Binary  \x00\x1a\x00";
    const uint32_t version = 7500;
    std::vector<uint8_t> data(64, 0);
    std::memcpy(data.data(), magic, 23);
    std::memcpy(data.data() + 23, &version, 4);
    const uint64_t hostileEnd = std::numeric_limits<uint64_t>::max();
    std::memcpy(data.data() + 27, &hostileEnd, sizeof(hostileEnd));

    const auto result = importer.ImportFromMemory(data.data(), data.size());
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, std::string("Malformed FBX node tree"));
}

TEST(FBXBinaryReader_OverflowingCursorOperationsFailClosed)
{
    const uint8_t data[8] = {};
    FBXBinaryReader reader(data, sizeof(data));
    EXPECT_FALSE(reader.Skip(std::numeric_limits<size_t>::max()));
    EXPECT_FALSE(reader.IsValid());
    EXPECT_FALSE(reader.HasRemaining());
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
