// TestGamePackager.cpp - Tests for Spark::Build::GamePackager
#include "TestFramework.h"
#include "Engine/Build/GamePackager.h"

#include <filesystem>
#include <fstream>

// Helper: create a temp file
static std::string CreateTempFile(const std::string& dir, const std::string& name, const std::string& content)
{
    std::filesystem::create_directories(dir);
    std::string path = dir + "/" + name;
    std::ofstream out(path);
    out << content;
    return path;
}

static void CleanupDir(const std::string& dir)
{
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ============================================================================
// Initialization
// ============================================================================

TEST(GamePackager_Initialize)
{
    auto& pkg = Spark::Build::GamePackager::GetInstance();
    pkg.Initialize();
    EXPECT_EQ(pkg.GetPackageCount(), static_cast<uint32_t>(0));
    pkg.Shutdown();
}

// ============================================================================
// Config validation
// ============================================================================

TEST(GamePackager_ValidateConfig_EmptyName)
{
    auto& pkg = Spark::Build::GamePackager::GetInstance();
    pkg.Initialize();
    Spark::Build::PackageConfig config;
    config.projectName = "";
    auto errors = pkg.ValidateConfig(config);
    EXPECT_TRUE(errors.size() > 0);
    pkg.Shutdown();
}

TEST(GamePackager_ValidateConfig_MissingExe)
{
    auto& pkg = Spark::Build::GamePackager::GetInstance();
    pkg.Initialize();
    Spark::Build::PackageConfig config;
    config.projectName = "Test";
    config.executablePath = "/nonexistent/game.exe";
    auto errors = pkg.ValidateConfig(config);
    EXPECT_TRUE(errors.size() > 0);
    pkg.Shutdown();
}

// ============================================================================
// Packaging
// ============================================================================

TEST(GamePackager_Package_Success)
{
    std::string tmpDir = "/tmp/spark_test_packager";
    CleanupDir(tmpDir);

    // Create fake build output
    auto exePath = CreateTempFile(tmpDir + "/build", "game.exe", "fake_exe_data");
    CreateTempFile(tmpDir + "/assets", "texture.png", "fake_texture");
    CreateTempFile(tmpDir + "/assets/models", "hero.obj", "fake_model");

    auto& pkg = Spark::Build::GamePackager::GetInstance();
    pkg.Initialize();

    Spark::Build::PackageConfig config;
    config.projectName = "TestGame";
    config.executablePath = exePath;
    config.outputDirectory = tmpDir + "/output";
    config.assetDirectory = tmpDir + "/assets";
    config.moduleDirectory = ""; // No modules for this test
    config.dataDirectory = "";

    auto result = pkg.Package(config);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.filesCopied >= 3); // exe + 2 assets
    EXPECT_TRUE(result.totalSizeBytes > 0);
    EXPECT_TRUE(result.durationSeconds >= 0.0);
    EXPECT_EQ(pkg.GetPackageCount(), static_cast<uint32_t>(1));

    // Verify output exists
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::exists(result.outputPath, ec));

    CleanupDir(tmpDir);
    pkg.Shutdown();
}

TEST(GamePackager_Package_MissingExe_Fails)
{
    auto& pkg = Spark::Build::GamePackager::GetInstance();
    pkg.Initialize();

    Spark::Build::PackageConfig config;
    config.projectName = "Test";
    config.executablePath = "/nonexistent/game.exe";
    config.outputDirectory = "/tmp/spark_test_pkg_fail";

    auto result = pkg.Package(config);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.errorMessage.find("not found") != std::string::npos);
    pkg.Shutdown();
}

// ============================================================================
// Platform extensions
// ============================================================================

TEST(GamePackager_ModuleExtension_Windows)
{
    auto ext = Spark::Build::GamePackager::GetModuleExtension(Spark::Build::PackagePlatform::WindowsX64);
    EXPECT_EQ(ext, std::string(".dll"));
}

TEST(GamePackager_ModuleExtension_Linux)
{
    auto ext = Spark::Build::GamePackager::GetModuleExtension(Spark::Build::PackagePlatform::LinuxX64);
    EXPECT_EQ(ext, std::string(".so"));
}

TEST(GamePackager_ModuleExtension_MacOS)
{
    auto ext = Spark::Build::GamePackager::GetModuleExtension(Spark::Build::PackagePlatform::MacOSX64);
    EXPECT_EQ(ext, std::string(".dylib"));
}

TEST(GamePackager_ExecutableExtension)
{
    auto win = Spark::Build::GamePackager::GetExecutableExtension(Spark::Build::PackagePlatform::WindowsX64);
    EXPECT_EQ(win, std::string(".exe"));
    auto lin = Spark::Build::GamePackager::GetExecutableExtension(Spark::Build::PackagePlatform::LinuxX64);
    EXPECT_EQ(lin, std::string(""));
}

TEST(GamePackager_ConsoleGetStatus)
{
    auto& pkg = Spark::Build::GamePackager::GetInstance();
    pkg.Initialize();
    auto status = pkg.Console_GetStatus();
    EXPECT_TRUE(status.find("Packages built: 0") != std::string::npos);
    pkg.Shutdown();
}
