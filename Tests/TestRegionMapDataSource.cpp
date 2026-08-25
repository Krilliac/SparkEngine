/**
 * @file TestRegionMapDataSource.cpp
 * @brief Terrafront Region Map Editor continent-source discovery tests.
 */

#include "TestFramework.h"
#include "Panels/RegionMapDataSource.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using SparkEditor::IsSafeRegionMapFileName;
using SparkEditor::LoadRegionMapDataSources;
using SparkEditor::RegionMapDataSource;
using SparkEditor::WriteRegionMapDocumentAtomically;

namespace
{
    fs::path SourceDataDirectory()
    {
#ifdef SPARK_TEST_SOURCE_DIR
        return fs::path(SPARK_TEST_SOURCE_DIR) / "Assets" / "MMOFPS" / "Data";
#else
        return fs::current_path() / "Assets" / "MMOFPS" / "Data";
#endif
    }

    fs::path ScratchDirectory(const std::string& stem)
    {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        return fs::temp_directory_path() / ("spark_region_map_" + stem + "_" + std::to_string(tick));
    }

    std::string ReadText(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        std::ostringstream bytes;
        bytes << input.rdbuf();
        return bytes.str();
    }
} // namespace

TEST(RegionMapDataSource_AcceptsPlainJsonAndRejectsTraversal)
{
    EXPECT_TRUE(IsSafeRegionMapFileName("regions.json"));
    EXPECT_TRUE(IsSafeRegionMapFileName("regions_highlands.json"));
    EXPECT_FALSE(IsSafeRegionMapFileName("../regions.json"));
    EXPECT_FALSE(IsSafeRegionMapFileName("subdir/regions.json"));
    EXPECT_FALSE(IsSafeRegionMapFileName("subdir\\regions.json"));
    EXPECT_FALSE(IsSafeRegionMapFileName("C:\\regions.json"));
    EXPECT_FALSE(IsSafeRegionMapFileName("regions.scene"));
}

TEST(RegionMapDataSource_DiscoversBothShippedContinents)
{
    std::vector<RegionMapDataSource> sources;
    std::string error;
    EXPECT_TRUE(LoadRegionMapDataSources(SourceDataDirectory(), sources, error));
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(sources.size(), size_t{2});
    EXPECT_EQ(sources[0].key, std::string("cindral_wastes"));
    EXPECT_EQ(sources[0].regionsFile, std::string("regions.json"));
    EXPECT_EQ(sources[1].key, std::string("veyra_highlands"));
    EXPECT_EQ(sources[1].regionsFile, std::string("regions_highlands.json"));
    EXPECT_TRUE(fs::is_regular_file(sources[0].dataPath));
    EXPECT_TRUE(fs::is_regular_file(sources[1].dataPath));
    EXPECT_FALSE(sources[0].dataPath == sources[1].dataPath);
}

TEST(RegionMapDataSource_RejectsUnsafeRegistryEntry)
{
    const fs::path scratch = ScratchDirectory("source");
    std::error_code ec;
    fs::create_directories(scratch, ec);
    {
        std::ofstream registry(scratch / "continents.json", std::ios::binary | std::ios::trunc);
        registry << R"({"continents":[{"mapId":1,"key":"bad","name":"Bad","regions":"../escape.json"}]})";
    }

    std::vector<RegionMapDataSource> sources;
    std::string error;
    EXPECT_FALSE(LoadRegionMapDataSources(scratch, sources, error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(sources.empty());
    fs::remove_all(scratch, ec);
}

TEST(RegionMapPersistence_CreatesFreshDocumentAtomically)
{
    const fs::path scratch = ScratchDirectory("fresh");
    const fs::path destination = scratch / "regions.json";
    std::error_code ec;
    fs::create_directories(scratch, ec);
    const std::string document = R"({"continent":{"name":"Fresh"},"regions":[]})";

    std::string error;
    EXPECT_TRUE(WriteRegionMapDocumentAtomically(destination, document, error));
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(ReadText(destination), document);
    EXPECT_FALSE(fs::exists(destination.string() + ".bak"));
    fs::remove_all(scratch, ec);
}

TEST(RegionMapPersistence_ExistingDocumentGetsExactBackup)
{
    const fs::path scratch = ScratchDirectory("existing");
    const fs::path destination = scratch / "regions_highlands.json";
    std::error_code ec;
    fs::create_directories(scratch, ec);
    const std::string oldDocument = R"({"continent":{"name":"Old"}})";
    const std::string newDocument = R"({"continent":{"name":"Veyra Highlands"},"regions":[]})";
    {
        std::ofstream output(destination, std::ios::binary);
        output << oldDocument;
    }

    std::string error;
    EXPECT_TRUE(WriteRegionMapDocumentAtomically(destination, newDocument, error));
    EXPECT_EQ(ReadText(destination), newDocument);
    EXPECT_EQ(ReadText(destination.string() + ".bak"), oldDocument);
    fs::remove_all(scratch, ec);
}

TEST(RegionMapPersistence_AcceptsAdditiveMixedVersionKeys)
{
    const fs::path scratch = ScratchDirectory("mixed");
    const fs::path destination = scratch / "regions.json";
    std::error_code ec;
    fs::create_directories(scratch, ec);
    const std::string document =
        R"({"$schema_note":"future","continent":{"name":"Cindral Wastes","futureFlag":true},"regions":[],"futureRoot":{"version":2}})";

    std::string error;
    EXPECT_TRUE(WriteRegionMapDocumentAtomically(destination, document, error));
    EXPECT_EQ(ReadText(destination), document);
    fs::remove_all(scratch, ec);
}

TEST(RegionMapPersistence_MalformedDocumentLeavesPrimaryAndBackupUntouched)
{
    const fs::path scratch = ScratchDirectory("malformed");
    const fs::path destination = scratch / "regions.json";
    const fs::path backup = scratch / "regions.json.bak";
    std::error_code ec;
    fs::create_directories(scratch, ec);
    const std::string oldDocument = R"({"continent":{"name":"Safe"}})";
    const std::string oldBackup = R"({"continent":{"name":"Earlier"}})";
    {
        std::ofstream output(destination, std::ios::binary);
        output << oldDocument;
        std::ofstream backupOutput(backup, std::ios::binary);
        backupOutput << oldBackup;
    }

    std::string error;
    EXPECT_FALSE(WriteRegionMapDocumentAtomically(destination, R"({"continent":)", error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(ReadText(destination), oldDocument);
    EXPECT_EQ(ReadText(backup), oldBackup);
    fs::remove_all(scratch, ec);
}
