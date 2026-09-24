// TestASSET220GamePackagerReal.cpp - ASSET-220: a package whose file copies
// fail must not be reported as a successful, publishable package.
#include "TestFramework.h"
#include "Engine/Build/GamePackager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    namespace fs = std::filesystem;

    void WriteFile(const fs::path& path, const std::string& content)
    {
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out << content;
    }

    void RemoveTree(const fs::path& path)
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    class ScopedCurrentPath
    {
      public:
        explicit ScopedCurrentPath(const fs::path& path) : m_previous(fs::current_path()) { fs::current_path(path); }
        ~ScopedCurrentPath()
        {
            std::error_code ec;
            fs::current_path(m_previous, ec);
        }
        ScopedCurrentPath(const ScopedCurrentPath&) = delete;
        ScopedCurrentPath& operator=(const ScopedCurrentPath&) = delete;

      private:
        fs::path m_previous;
    };
} // namespace

TEST(GamePackager_ASSET220_CopyFailureFailsPackage)
{
    const fs::path tmpDir = fs::temp_directory_path() / "spark_asset220_package_copy_failure";
    RemoveTree(tmpDir);
    const fs::path exePath = tmpDir / "build" / "game.exe";
    WriteFile(exePath, "fake_exe_data");
    WriteFile(tmpDir / "assets" / "ok.txt", "ok");
    WriteFile(tmpDir / "assets" / "blocked.txt", "blocked");

    // A directory occupying the destination makes copy_file fail portably.
    const fs::path outputRoot = tmpDir / "output" / "BlockedGame";
    fs::create_directories(outputRoot / "Assets" / "blocked.txt");

    auto& packager = Spark::Build::GamePackager::GetInstance();
    packager.Initialize();
    Spark::Build::PackageConfig config;
    config.projectName = "BlockedGame";
    config.executablePath = exePath.string();
    config.outputDirectory = (tmpDir / "output").string();
    config.assetDirectory = (tmpDir / "assets").string();
    config.moduleDirectory = "";
    config.dataDirectory = "";

    const auto result = packager.Package(config);

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.errorMessage.find("Failed to copy") != std::string::npos);
    EXPECT_TRUE(result.errorMessage.find("blocked.txt") != std::string::npos);
    EXPECT_TRUE(result.outputPath.empty());
    // The two copies that did succeed are still reported for diagnostics.
    EXPECT_EQ(result.filesCopied, static_cast<uint32_t>(2));
    EXPECT_EQ(packager.GetPackageCount(), static_cast<uint32_t>(0));
    EXPECT_FALSE(packager.GetLastResult().success);
    EXPECT_TRUE(packager.Console_GetStatus().find("Last:") == std::string::npos);

    packager.Shutdown();
    RemoveTree(tmpDir);
}

TEST(GamePackager_ASSET220_EveryCopyFailureIsReported)
{
    const fs::path tmpDir = fs::temp_directory_path() / "spark_asset220_package_copy_failures";
    RemoveTree(tmpDir);
    const fs::path exePath = tmpDir / "build" / "game.exe";
    WriteFile(exePath, "fake_exe_data");
    WriteFile(tmpDir / "assets" / "first.txt", "1");
    WriteFile(tmpDir / "assets" / "second.txt", "2");

    const fs::path outputRoot = tmpDir / "output" / "BlockedTwice";
    fs::create_directories(outputRoot / "Assets" / "first.txt");
    fs::create_directories(outputRoot / "Assets" / "second.txt");

    auto& packager = Spark::Build::GamePackager::GetInstance();
    packager.Initialize();
    Spark::Build::PackageConfig config;
    config.projectName = "BlockedTwice";
    config.executablePath = exePath.string();
    config.outputDirectory = (tmpDir / "output").string();
    config.assetDirectory = (tmpDir / "assets").string();
    config.moduleDirectory = "";
    config.dataDirectory = "";

    const auto result = packager.Package(config);

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.errorMessage.find("first.txt") != std::string::npos);
    EXPECT_TRUE(result.errorMessage.find("second.txt") != std::string::npos);
    EXPECT_EQ(result.filesCopied, static_cast<uint32_t>(1)); // only the executable

    packager.Shutdown();
    RemoveTree(tmpDir);
}

TEST(GamePackager_ASSET220_CleanPackageStillSucceeds)
{
    const fs::path tmpDir = fs::temp_directory_path() / "spark_asset220_package_clean";
    RemoveTree(tmpDir);
    const fs::path exePath = tmpDir / "build" / "game.exe";
    WriteFile(exePath, "fake_exe_data");
    WriteFile(tmpDir / "assets" / "ok.txt", "ok");

    auto& packager = Spark::Build::GamePackager::GetInstance();
    packager.Initialize();
    Spark::Build::PackageConfig config;
    config.projectName = "CleanGame";
    config.executablePath = exePath.string();
    config.outputDirectory = (tmpDir / "output").string();
    config.assetDirectory = (tmpDir / "assets").string();
    config.moduleDirectory = "";
    config.dataDirectory = "";

    const auto result = packager.Package(config);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.errorMessage.empty());
    EXPECT_EQ(result.filesCopied, static_cast<uint32_t>(2));
    EXPECT_FALSE(result.outputPath.empty());
    EXPECT_EQ(packager.GetPackageCount(), static_cast<uint32_t>(1));

    packager.Shutdown();
    RemoveTree(tmpDir);
}

TEST(GamePackager_ASSET220_LegacyAssetCopyFailureFailsPackage)
{
    const fs::path tmpDir = fs::temp_directory_path() / "spark_asset220_legacy_asset_failure";
    RemoveTree(tmpDir);
    WriteFile(tmpDir / "build" / "Release" / "Game.dll", "binary");
    WriteFile(tmpDir / "Assets" / "ok.txt", "ok");
    WriteFile(tmpDir / "Assets" / "blocked.txt", "blocked");

    ScopedCurrentPath cwd(tmpDir);
    const fs::path outputRoot = tmpDir / "Package" / "LegacyBlocked_Windows_Release";
    fs::create_directories(outputRoot / "Assets" / "blocked.txt");

    auto& packager = Spark::Build::GamePackager::GetInstance();
    packager.Initialize();
    Spark::Build::LegacyPackageConfig config;
    config.projectName = "LegacyBlocked";
    config.outputDirectory = "Package";
    const auto result = packager.PackageLegacy(config);
    packager.Shutdown();

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.outputPath.empty());
    EXPECT_TRUE(std::any_of(result.errors.begin(), result.errors.end(), [](const std::string& error)
                            { return error.starts_with("Failed to copy asset 'blocked.txt': "); }));
    EXPECT_TRUE(std::none_of(result.warnings.begin(), result.warnings.end(), [](const std::string& warning)
                             { return warning.find("Failed to copy") != std::string::npos; }));
    EXPECT_FALSE(fs::exists(outputRoot / "manifest.txt"));
    EXPECT_EQ(result.assetCount, static_cast<uint32_t>(1));
    EXPECT_EQ(result.dllCount, static_cast<uint32_t>(1));
}
