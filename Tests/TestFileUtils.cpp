// TestFileUtils.cpp - Tests for file I/O and path utilities
// Uses the actual FileUtils.h header

#include "TestFramework.h"
#include "Utils/FileUtils.h"
#include <cstdio>
#include <filesystem>

using namespace Spark::FileUtils;

static std::string GetTempDir()
{
    return std::filesystem::temp_directory_path().string();
}

// =============================================================================
// Path Manipulation Tests
// =============================================================================

TEST(FileUtils_GetExtension)
{
    EXPECT_EQ(GetExtension("model.fbx"), std::string(".fbx"));
    EXPECT_EQ(GetExtension("archive.tar.gz"), std::string(".gz"));
    EXPECT_EQ(GetExtension("noext"), std::string(""));
}

TEST(FileUtils_GetFilename)
{
    EXPECT_EQ(GetFilename("path/to/model.fbx"), std::string("model.fbx"));
    EXPECT_EQ(GetFilename("model.fbx"), std::string("model.fbx"));
}

TEST(FileUtils_GetStem)
{
    EXPECT_EQ(GetStem("path/to/model.fbx"), std::string("model"));
    EXPECT_EQ(GetStem("noext"), std::string("noext"));
}

TEST(FileUtils_GetDirectory)
{
    EXPECT_EQ(GetDirectory("path/to/model.fbx"), std::string("path/to"));
}

TEST(FileUtils_JoinPath)
{
    std::string joined = JoinPath("assets", "model.fbx");
    // Should contain both parts with a separator
    EXPECT_TRUE(joined.find("assets") != std::string::npos);
    EXPECT_TRUE(joined.find("model.fbx") != std::string::npos);
}

#if SPARK_HAS_FILESYSTEM

TEST(FileUtils_ChangeExtension)
{
    std::string changed = ChangeExtension("model.fbx", ".obj");
    EXPECT_TRUE(changed.find(".obj") != std::string::npos);
    EXPECT_TRUE(changed.find(".fbx") == std::string::npos);
}

TEST(FileUtils_NormalizePath)
{
    std::string norm = NormalizePath("a/b/../c");
    EXPECT_TRUE(norm.find("..") == std::string::npos);
}

#endif

// =============================================================================
// File I/O Tests
// =============================================================================

TEST(FileUtils_WriteAndReadText)
{
    std::string path = GetTempDir() + "/spark_test_text.txt";
    std::string content = "Hello, SparkEngine!\nLine 2.";

    EXPECT_TRUE(WriteTextFile(path, content));

    auto result = ReadTextFile(path);
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), content);

    std::remove(path.c_str());
}

TEST(FileUtils_WriteAndReadBinary)
{
    std::string path = GetTempDir() + "/spark_test_binary.bin";
    std::vector<uint8_t> data = {0x00, 0x01, 0x02, 0xFF, 0xFE, 0xFD};

    EXPECT_TRUE(WriteBinaryFile(path, data));

    auto result = ReadBinaryFile(path);
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ((int)result.value().size(), (int)data.size());
    for (int i = 0; i < (int)data.size(); ++i)
    {
        EXPECT_EQ(result.value()[i], data[i]);
    }

    std::remove(path.c_str());
}

TEST(FileUtils_ReadNonexistent)
{
    auto result = ReadTextFile(GetTempDir() + "/spark_nonexistent_file_xyz.txt");
    EXPECT_FALSE(result.has_value());
}

#if SPARK_HAS_FILESYSTEM

TEST(FileUtils_FileExists)
{
    std::string path = GetTempDir() + "/spark_test_exists.txt";
    WriteTextFile(path, "test");
    EXPECT_TRUE(FileExists(path));
    std::remove(path.c_str());
    EXPECT_FALSE(FileExists(path));
}

TEST(FileUtils_IsDirectory)
{
    EXPECT_TRUE(IsDirectory(GetTempDir()));
    EXPECT_FALSE(IsDirectory(GetTempDir() + "/spark_nonexistent_dir_xyz"));
}

TEST(FileUtils_GetFileSize)
{
    std::string path = GetTempDir() + "/spark_test_size.txt";
    WriteTextFile(path, "12345");

    auto size = GetFileSize(path);
    EXPECT_TRUE(size.has_value());
    EXPECT_EQ(size.value(), (uintmax_t)5);

    std::remove(path.c_str());
}

TEST(FileUtils_CreateDirectories)
{
    std::string dir = GetTempDir() + "/spark_test_dir/sub/deep";
    EXPECT_TRUE(CreateDirectories(dir));
    EXPECT_TRUE(IsDirectory(dir));

    // Cleanup
    std::error_code ec;
    fs::remove_all(GetTempDir() + "/spark_test_dir", ec);
}

TEST(FileUtils_ListFiles)
{
    std::string dir = GetTempDir() + "/spark_test_list";
    CreateDirectories(dir);
    WriteTextFile(dir + "/a.txt", "a");
    WriteTextFile(dir + "/b.txt", "b");
    WriteTextFile(dir + "/c.dat", "c");

    auto all = ListFiles(dir);
    EXPECT_EQ((int)all.size(), 3);

    auto txtOnly = ListFiles(dir, ".txt");
    EXPECT_EQ((int)txtOnly.size(), 2);

    // Cleanup
    std::error_code ec;
    fs::remove_all(dir, ec);
}

#endif
