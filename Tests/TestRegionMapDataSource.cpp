/**
 * @file TestRegionMapDataSource.cpp
 * @brief Terrafront Region Map Editor continent-source discovery tests.
 */

#include "TestFramework.h"
#include "Panels/RegionMapDataSource.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using SparkEditor::IsSafeRegionMapFileName;
using SparkEditor::LoadRegionMapDataSources;
using SparkEditor::RegionMapDataSource;

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
}

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
    const fs::path scratch = fs::temp_directory_path() / "spark_region_map_source_test";
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
