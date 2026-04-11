/**
 * @file TestFileUtilsReal.cpp
 * @brief Real-class tests for Spark::FileUtils namespace
 */

#include "TestFramework.h"
#include "Utils/FileUtils.h"

#include <cstdio>
#include <filesystem>

namespace
{
    std::string UniqueTmpPath(const char* name)
    {
        return std::string("/tmp/sparkengine_phaseqq_") + name;
    }
} // namespace

TEST(FileUtilsReal_WriteReadTextRoundTrip)
{
    const auto path = UniqueTmpPath("text.txt");
    const std::string content = "Hello PhaseQQ\nLine 2\n";
    EXPECT_TRUE(Spark::FileUtils::WriteTextFile(path, content));

    auto read = Spark::FileUtils::ReadTextFile(path);
    EXPECT_TRUE(read.has_value());
    if (read)
        EXPECT_EQ(*read, content);

    std::remove(path.c_str());
}

TEST(FileUtilsReal_ReadMissingFileReturnsNullopt)
{
    auto read = Spark::FileUtils::ReadTextFile("/tmp/definitely_does_not_exist_phaseqq_123456789");
    EXPECT_FALSE(read.has_value());
}

TEST(FileUtilsReal_WriteReadBinaryRoundTrip)
{
    const auto path = UniqueTmpPath("bin.dat");
    const std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03};
    EXPECT_TRUE(Spark::FileUtils::WriteBinaryFile(path, data));

    auto read = Spark::FileUtils::ReadBinaryFile(path);
    EXPECT_TRUE(read.has_value());
    if (read)
    {
        EXPECT_EQ(read->size(), data.size());
        for (size_t i = 0; i < data.size(); ++i)
            EXPECT_EQ((*read)[i], data[i]);
    }

    std::remove(path.c_str());
}

TEST(FileUtilsReal_FileExists)
{
    const auto path = UniqueTmpPath("exists.txt");
    EXPECT_FALSE(Spark::FileUtils::FileExists(path));
    Spark::FileUtils::WriteTextFile(path, "x");
    EXPECT_TRUE(Spark::FileUtils::FileExists(path));
    std::remove(path.c_str());
    EXPECT_FALSE(Spark::FileUtils::FileExists(path));
}

TEST(FileUtilsReal_GetExtension)
{
    EXPECT_EQ(Spark::FileUtils::GetExtension("file.txt"), std::string(".txt"));
    EXPECT_EQ(Spark::FileUtils::GetExtension("path/to/mesh.obj"), std::string(".obj"));
    EXPECT_EQ(Spark::FileUtils::GetExtension("noext"), std::string(""));
}

TEST(FileUtilsReal_GetFilenameStem)
{
    EXPECT_EQ(Spark::FileUtils::GetFilename("/path/to/file.txt"), std::string("file.txt"));
    EXPECT_EQ(Spark::FileUtils::GetStem("/path/to/file.txt"), std::string("file"));
}

TEST(FileUtilsReal_GetDirectory)
{
    EXPECT_EQ(Spark::FileUtils::GetDirectory("/path/to/file.txt"), std::string("/path/to"));
}
