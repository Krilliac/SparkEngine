/** @file TestLauncherPaths.cpp @brief SparkLauncher path discovery and project-list layout contract tests. */
#include "TestFramework.h"
#include "../SparkLauncher/src/LauncherPaths.h"
#include "../SparkLauncher/src/LauncherViews.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace
{
    std::filesystem::path MakeLauncherPathsTestRoot()
    {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        const auto root = std::filesystem::temp_directory_path() / ("spark-launcher-paths-" + std::to_string(stamp));
        std::filesystem::create_directories(root);
        return root;
    }
} // namespace

TEST(LauncherPaths_FindsInstalledTemplatesRelativeToExecutable)
{
    const auto root = MakeLauncherPathsTestRoot();
    const auto executable = root / "install" / "bin" / "SparkLauncher";
    const auto installedTemplates = root / "install" / "share" / "SparkEngine" / "templates";
    const auto unrelatedWorkingDirectory = root / "working" / "project";
    std::filesystem::create_directories(installedTemplates);
    std::filesystem::create_directories(unrelatedWorkingDirectory);

    const auto found = SparkLauncher::FindLauncherTemplatesDirectory(executable, unrelatedWorkingDirectory);
    EXPECT_EQ(found, std::filesystem::canonical(installedTemplates));

    std::error_code error;
    std::filesystem::remove_all(root, error);
    EXPECT_FALSE(error);
}

TEST(LauncherPaths_PrefersInstalledTemplatesOverWorkingDirectory)
{
    const auto root = MakeLauncherPathsTestRoot();
    const auto executable = root / "install" / "bin" / "SparkLauncher";
    const auto developmentTemplates = root / "source" / "Templates";
    const auto installedTemplates = root / "install" / "share" / "SparkEngine" / "templates";
    std::filesystem::create_directories(developmentTemplates);
    std::filesystem::create_directories(installedTemplates);

    const auto found = SparkLauncher::FindLauncherTemplatesDirectory(executable, root / "source");
    EXPECT_EQ(found, std::filesystem::canonical(installedTemplates));

    std::error_code error;
    std::filesystem::remove_all(root, error);
    EXPECT_FALSE(error);
}

TEST(LauncherPaths_FallsBackToDevelopmentTemplates)
{
    const auto root = MakeLauncherPathsTestRoot();
    const auto executable = root / "build" / "bin" / "SparkLauncher";
    const auto developmentTemplates = root / "source" / "Templates";
    std::filesystem::create_directories(developmentTemplates);

    const auto found = SparkLauncher::FindLauncherTemplatesDirectory(executable, root / "source");
    EXPECT_EQ(found, std::filesystem::canonical(developmentTemplates));

    std::error_code error;
    std::filesystem::remove_all(root, error);
    EXPECT_FALSE(error);
}

TEST(LauncherPaths_ResolvesRunningExecutable)
{
    const auto executable = SparkLauncher::GetLauncherExecutablePath();
    EXPECT_FALSE(executable.empty());
    EXPECT_TRUE(std::filesystem::is_regular_file(executable));
}

// The launcher's layout rules (LauncherViews.h, LauncherLayout) are ImGui-free so they are checked here with a
// one-unit-per-byte measure instead of a font.

TEST(LauncherLayout_ProjectRowColumnsAreTheSameForMissingRows)
{
    // Row 0..1000, 12 px padding, 8 px spacing, 140 px metadata, primary button as wide as "Locate...", 30 px square.
    const auto columns =
        SparkLauncher::LauncherLayout::ComputeProjectRowColumns(0.0f, 1000.0f, 12.0f, 8.0f, 140.0f, 90.0f, 30.0f);
    EXPECT_NEAR(columns.secondaryX, 1000.0f - 12.0f - 30.0f, 1e-4f);
    EXPECT_NEAR(columns.primaryX + columns.primaryWidth + 8.0f, columns.secondaryX, 1e-4f);
    EXPECT_NEAR(columns.primaryWidth, 90.0f, 1e-4f);
    EXPECT_NEAR(columns.metaX + 140.0f + 12.0f, columns.primaryX, 1e-4f);
    EXPECT_NEAR(columns.textX, 12.0f, 1e-4f);
    EXPECT_NEAR(columns.textX + columns.textWidth + 12.0f, columns.metaX, 1e-4f);

    // A row too narrow for the fixed columns gives the name and path no width instead of a negative one.
    const auto narrow =
        SparkLauncher::LauncherLayout::ComputeProjectRowColumns(0.0f, 200.0f, 12.0f, 8.0f, 140.0f, 90.0f, 30.0f);
    EXPECT_NEAR(narrow.textWidth, 0.0f, 1e-4f);
}

TEST(LauncherLayout_EllipsizePathCutsWholeFolderNames)
{
    using SparkLauncher::LauncherLayout::EllipsizePath;
    const auto bytes = [](std::string_view text) { return static_cast<float>(text.size()); };

    // Fits: unchanged.
    EXPECT_EQ(EllipsizePath("D:/Spark/bin", 40.0f, bytes), std::string("D:/Spark/bin"));

    // The root stays, the longest tail of whole folder names that fits follows the ellipsis.
    const std::string engine = EllipsizePath("D:/SparkEngine-pr616-20261005/build/native/bin/Release", 26.0f, bytes);
    EXPECT_EQ(engine, std::string("D:/.../native/bin/Release"));

    // Windows separators are kept as they are.
    const std::string project = EllipsizePath("C:\\Users\\dev\\Projects\\Game\\Game.sparkproject", 30.0f, bytes);
    EXPECT_EQ(project, std::string("C:\\...\\Game\\Game.sparkproject"));

    // When even the last folder does not fit beside the root, the root is dropped.
    EXPECT_EQ(EllipsizePath("D:/a/VeryLongFolderName", 22.0f, bytes), std::string(".../VeryLongFolderName"));

    // A single over-long name falls back to a middle cut that still fits.
    const std::string single = EllipsizePath("ThisIsAVeryLongFileName.sparkproject", 12.0f, bytes);
    EXPECT_LE(single.size(), std::size_t{12});
    EXPECT_STR_CONTAINS(single, "...");
}

TEST(LauncherLayout_MiddleEllipsisNeverSplitsACodePoint)
{
    using SparkLauncher::LauncherLayout::MiddleEllipsis;
    const auto bytes = [](std::string_view text) { return static_cast<float>(text.size()); };
    // "Übung" spelled with a two-byte U+00DC at both ends; every cut must leave whole code points.
    const std::string text = "\xc3\x9c"
                             "bungsprojekt-mit-langem-Namen-\xc3\x9c";
    for (float width = 3.0f; width < static_cast<float>(text.size()); width += 1.0f)
    {
        const std::string cut = MiddleEllipsis(text, width, bytes);
        EXPECT_LE(static_cast<float>(cut.size()), width);
        for (std::size_t i = 0; i < cut.size(); ++i)
        {
            const auto byte = static_cast<unsigned char>(cut[i]);
            if (byte == 0xC3u)
            {
                ASSERT_TRUE(i + 1 < cut.size());
                EXPECT_EQ(static_cast<unsigned char>(cut[i + 1]), static_cast<unsigned char>(0x9Cu));
            }
            if (byte == 0x9Cu)
            {
                ASSERT_TRUE(i > 0);
                EXPECT_EQ(static_cast<unsigned char>(cut[i - 1]), static_cast<unsigned char>(0xC3u));
            }
        }
    }
}
