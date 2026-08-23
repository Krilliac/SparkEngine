// Test_persistence_SaveSystem.cpp
// Regression for two SaveSystem findings:
//   P1: Load/ReadFromFile never validated the save-format version. A file written by a
//       newer, incompatible format is now rejected instead of silently misinterpreted.
//   P2: GetSaveMetadata now uses a metadata-only read path; this test also confirms it
//       still parses the metadata header correctly.
// Both are exercised through the public GetSaveMetadata() (which needs no World/ECS),
// by hand-crafting .spark_save files with the real on-disk binary layout.

#include "TestFramework.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Engine/ECS/Components.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace Spark;

namespace
{
    // Write a minimal but format-correct save file (header + metadata block only; zero
    // entities are not required because GetSaveMetadata stops after the metadata block).
    void WriteSaveHeader(const std::string& path, uint32_t version, const std::string& metaStr)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write("SPRK", 4);
        out.write(reinterpret_cast<const char*>(&version), sizeof(version));
        uint32_t metaSize = static_cast<uint32_t>(metaStr.size());
        out.write(reinterpret_cast<const char*>(&metaSize), sizeof(metaSize));
        out.write(metaStr.data(), static_cast<std::streamsize>(metaStr.size()));
    }

    std::string MakeTempSaveDir(const char* name)
    {
        auto dir = std::filesystem::temp_directory_path() / (std::string("spark_harden_save_") + name);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir.string();
    }

    // Metadata layout: three getline fields, then whitespace-separated
    // timestamp playTime health armor posX posY posZ kills deaths.
    const std::string kValidMeta = "My Save\nLevel1\nSoldier\n1234 56.5 100 50 1 2 3 4 5\n";
} // namespace

TEST(SaveSystem_GetSaveMetadata_ParsesHeader)
{
    const std::string dir = MakeTempSaveDir("valid");
    SaveSystem& ss = SaveSystem::GetInstance();
    ss.SetSaveDirectory(dir);

    WriteSaveHeader(dir + "/goodslot.spark_save", 1u, kValidMeta);

    SaveMetadata meta;
    EXPECT_TRUE(ss.GetSaveMetadata("goodslot", meta));
    EXPECT_EQ(meta.saveName, std::string("My Save"));
    EXPECT_EQ(meta.sceneName, std::string("Level1"));
    EXPECT_EQ(meta.playerClass, std::string("Soldier"));
    EXPECT_EQ(meta.timestamp, static_cast<uint64_t>(1234));
    EXPECT_EQ(meta.version, 1u);

    std::filesystem::remove_all(dir);
}

TEST(SaveSystem_GetSaveMetadata_RejectsNewerVersion)
{
    const std::string dir = MakeTempSaveDir("newer");
    SaveSystem& ss = SaveSystem::GetInstance();
    ss.SetSaveDirectory(dir);

    // Version far higher than any supported format: must be refused, not parsed.
    WriteSaveHeader(dir + "/futureslot.spark_save", 999u, kValidMeta);

    SaveMetadata meta;
    EXPECT_FALSE(ss.GetSaveMetadata("futureslot", meta));

    std::filesystem::remove_all(dir);
}

TEST(SaveSystem_GetSaveMetadata_RejectsBadMagic)
{
    const std::string dir = MakeTempSaveDir("badmagic");
    SaveSystem& ss = SaveSystem::GetInstance();
    ss.SetSaveDirectory(dir);

    {
        std::ofstream out(dir + "/junkslot.spark_save", std::ios::binary | std::ios::trunc);
        const char junk[] = "NOPExxxxxxxx";
        out.write(junk, sizeof(junk) - 1);
    }

    SaveMetadata meta;
    EXPECT_FALSE(ss.GetSaveMetadata("junkslot", meta));

    std::filesystem::remove_all(dir);
}

TEST(SaveSystem_Load_RejectsTruncatedCustomStateCountWithoutChangingWorld)
{
    const std::string dir = MakeTempSaveDir("truncated_tail");
    SaveSystem& ss = SaveSystem::GetInstance();
    EXPECT_TRUE(ss.Initialize(dir));

    World source;
    SaveMetadata metadata;
    metadata.saveName = "Tail test";
    EXPECT_TRUE(ss.Save("tailslot", source, metadata));

    const auto path = std::filesystem::path(dir) / "tailslot.spark_save";
    std::ifstream input(path, std::ios::binary);
    const std::vector<char> original((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    EXPECT_TRUE(original.size() >= sizeof(uint32_t));

    for (size_t bytesRemoved = 1; bytesRemoved <= sizeof(uint32_t); ++bytesRemoved)
    {
        std::ofstream truncated(path, std::ios::binary | std::ios::trunc);
        truncated.write(original.data(), static_cast<std::streamsize>(original.size() - bytesRemoved));
        truncated.close();

        World target;
        target.CreateEntity("sentinel");
        EXPECT_FALSE(ss.Load("tailslot", target));
        EXPECT_EQ(target.GetEntityCount(), 1u);
    }

    std::filesystem::remove_all(dir);
}

TEST(SaveSystem_Load_RejectsEveryTruncatedCustomStateField)
{
    const std::string dir = MakeTempSaveDir("truncated_custom_entry");
    SaveSystem& ss = SaveSystem::GetInstance();
    EXPECT_TRUE(ss.Initialize(dir));

    World source;
    SaveMetadata metadata;
    metadata.saveName = "Custom entry test";
    EXPECT_TRUE(ss.Save("customslot", source, metadata));

    const auto path = std::filesystem::path(dir) / "customslot.spark_save";
    std::ifstream input(path, std::ios::binary);
    std::vector<char> prefix((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    EXPECT_TRUE(prefix.size() >= sizeof(uint32_t));
    prefix.resize(prefix.size() - sizeof(uint32_t));

    auto append16 = [](std::vector<char>& bytes, uint16_t value)
    {
        bytes.push_back(static_cast<char>(value & 0xFF));
        bytes.push_back(static_cast<char>((value >> 8) & 0xFF));
    };
    auto append32 = [](std::vector<char>& bytes, uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<char>((value >> shift) & 0xFF));
    };

    std::vector<std::vector<char>> malformed;
    auto entry = prefix;
    append32(entry, 1);
    malformed.push_back(entry); // missing key length
    append16(entry, 2);
    malformed.push_back(entry); // missing key bytes
    entry.push_back('k');
    malformed.push_back(entry); // partial key bytes
    entry.push_back('2');
    malformed.push_back(entry); // missing value length
    append16(entry, 3);
    malformed.push_back(entry); // missing value bytes
    entry.push_back('v');
    malformed.push_back(entry); // partial value bytes

    for (const auto& bytes : malformed)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();

        World target;
        target.CreateEntity("sentinel");
        EXPECT_FALSE(ss.Load("customslot", target));
        EXPECT_EQ(target.GetEntityCount(), 1u);
    }

    std::filesystem::remove_all(dir);
}
