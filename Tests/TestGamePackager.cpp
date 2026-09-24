// TestGamePackager.cpp - Tests for Spark::Build::GamePackager
#include "TestFramework.h"
#include "Core/GamePackager.h"
#include "Engine/Build/GamePackager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>

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

class ScopedCurrentPath
{
  public:
    explicit ScopedCurrentPath(const std::filesystem::path& path) : m_previous(std::filesystem::current_path())
    {
        std::filesystem::current_path(path);
    }

    ~ScopedCurrentPath()
    {
        std::error_code ec;
        std::filesystem::current_path(m_previous, ec);
    }

    ScopedCurrentPath(const ScopedCurrentPath&) = delete;
    ScopedCurrentPath& operator=(const ScopedCurrentPath&) = delete;

  private:
    std::filesystem::path m_previous;
};

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

TEST(GamePackager_Package_RejectsPathTraversalName)
{
    const std::string tmpDir = (std::filesystem::temp_directory_path() / "spark_test_packager_validation").string();
    CleanupDir(tmpDir);
    const auto exePath = CreateTempFile(tmpDir, "game.exe", "fake_exe_data");

    auto& pkg = Spark::Build::GamePackager::GetInstance();
    pkg.Initialize();
    Spark::Build::PackageConfig config;
    config.projectName = "../escape";
    config.executablePath = exePath;
    config.outputDirectory = tmpDir + "/output";
    config.assetDirectory = "";
    config.dataDirectory = "";
    config.moduleDirectory = "";

    const auto result = pkg.Package(config);

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.errorMessage.find("safe path component") != std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(tmpDir) / "escape"));
    CleanupDir(tmpDir);
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
    EXPECT_TRUE(result.filesCopied >= 3);                    // exe + 2 assets
    EXPECT_EQ(result.filesCopied, static_cast<uint32_t>(3)); // manifest metadata is excluded
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

TEST(GamePackager_Package_UninitializedReportsAllConfigurationErrors)
{
    auto& pkg = Spark::GamePackager::GetInstance();
    pkg.Shutdown();

    Spark::PackageConfig config;
    config.outputDir.clear();
    config.projectName = "bad/name";
    const auto result = pkg.Package(config);

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errors.size(), static_cast<size_t>(3));
    EXPECT_TRUE(std::find(result.errors.begin(), result.errors.end(), "Output directory must not be empty") !=
                result.errors.end());
    EXPECT_TRUE(std::find(result.errors.begin(), result.errors.end(),
                          "Project name contains invalid filesystem characters") != result.errors.end());
    EXPECT_TRUE(std::find(result.errors.begin(), result.errors.end(), "GamePackager has not been initialized") !=
                result.errors.end());
}

static std::set<std::string> RelativeFiles(const std::filesystem::path& root)
{
    std::set<std::string> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec))
    {
        if (entry.is_regular_file(ec))
            files.insert(std::filesystem::relative(entry.path(), root, ec).generic_string());
    }
    return files;
}

static std::vector<std::string> NormalizedManifest(const std::filesystem::path& root)
{
    std::ifstream input(root / "manifest.txt");
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line))
    {
        if (line.starts_with("# Timestamp:"))
            line = "# Timestamp: <normalized>";
        lines.push_back(std::move(line));
    }
    return lines;
}

// The legacy Core call surface must produce the same staged package shape as
// the canonical Build call surface.  This prevents the two historical
// implementations from silently drifting again.
TEST(GamePackager_LegacyCoreFacadeMatchesCanonicalOutput)
{
    const auto tmpDir = std::filesystem::temp_directory_path() / "spark_test_packager_facade";
    CleanupDir(tmpDir.string());
    CreateTempFile((tmpDir / "build" / "Release").string(), "CompatGame.exe", "fake_exe_data");
    CreateTempFile((tmpDir / "build" / "Release").string(), "Renderer.dll", "fake_dll_data");
    CreateTempFile((tmpDir / "build" / "Release").string(), "Editor.dll", "editor_dll_data");
    CreateTempFile((tmpDir / "build" / "Release").string(), "CompatGame.pdb", "debug_symbols");
    CreateTempFile((tmpDir / "build" / "Release").string(), "readme.txt", "not_a_binary");
    CreateTempFile((tmpDir / "build" / "Debug").string(), "CompatGame.exe", "debug_exe_data");
    CreateTempFile((tmpDir / "build" / "Debug").string(), "Renderer.dll", "debug_dll_data");
    CreateTempFile((tmpDir / "build" / "Debug").string(), "CompatGame.pdb", "debug_symbols");
    CreateTempFile((tmpDir / "Assets").string(), "texture.png", "fake_texture");

    ScopedCurrentPath cwd(tmpDir);

    auto& legacy = Spark::GamePackager::GetInstance();
    legacy.Initialize();
    Spark::PackageConfig legacyConfig;
    legacyConfig.projectName = "CompatGame";
    legacyConfig.outputDir = "Package";
    legacyConfig.includeEditor = false;
    const auto legacyResult = legacy.Package(legacyConfig);
    const auto legacyStatus = legacy.Console_GetStatus();
    const auto legacyPlatforms = legacy.GetSupportedPlatforms();
    legacy.Shutdown();

    auto& canonical = Spark::Build::GamePackager::GetInstance();
    canonical.Initialize();
    Spark::Build::LegacyPackageConfig canonicalConfig;
    canonicalConfig.projectName = "CompatGame";
    canonicalConfig.outputDirectory = "CanonicalPackage";
    canonicalConfig.platform = Spark::Build::PackagePlatform::WindowsX64;
    canonicalConfig.debugBuild = false;
    canonicalConfig.stripDebugSymbols = true;
    canonicalConfig.compressAssets = true;
    canonicalConfig.includeEditor = false;
    const auto canonicalResult = canonical.PackageLegacy(canonicalConfig);
    EXPECT_EQ(canonical.GetPackageCount(), static_cast<uint32_t>(1));
    EXPECT_TRUE(canonical.Console_GetStatus().find("Packages built: 1") != std::string::npos);
    canonical.Shutdown();
    EXPECT_EQ(canonical.GetLastResult().filesCopied, static_cast<uint32_t>(3));

    EXPECT_TRUE(legacyResult.success);
    EXPECT_TRUE(canonicalResult.success);
    EXPECT_EQ(legacyPlatforms.size(), static_cast<size_t>(3));
#if defined(_WIN32)
    EXPECT_EQ(static_cast<int>(legacyPlatforms[0]), static_cast<int>(Spark::TargetPlatform::Windows));
#elif defined(__linux__)
    EXPECT_EQ(static_cast<int>(legacyPlatforms[0]), static_cast<int>(Spark::TargetPlatform::Linux));
#elif defined(__APPLE__)
    EXPECT_EQ(static_cast<int>(legacyPlatforms[0]), static_cast<int>(Spark::TargetPlatform::macOS));
#endif
    EXPECT_TRUE(legacyStatus.find("initialized, 3 supported platform(s)") != std::string::npos);
    EXPECT_TRUE(legacyStatus.find("Assets: 1, DLLs: 2") != std::string::npos);
    EXPECT_TRUE(std::filesystem::path(legacyResult.outputPath).is_absolute());
    EXPECT_TRUE(std::filesystem::path(canonicalResult.outputPath).is_absolute());
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(legacyResult.outputPath) / "Bin" / "CompatGame.exe"));
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(legacyResult.outputPath) / "Bin" / "Renderer.dll"));
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(legacyResult.outputPath) / "Bin" / "Editor.dll"));
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(legacyResult.outputPath) / "Bin" / "readme.txt"));
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(legacyResult.outputPath) / "Assets" / "texture.png"));
    EXPECT_TRUE(RelativeFiles(legacyResult.outputPath) == RelativeFiles(canonicalResult.outputPath));
    EXPECT_TRUE(NormalizedManifest(legacyResult.outputPath) == NormalizedManifest(canonicalResult.outputPath));
    EXPECT_EQ(RelativeFiles(legacyResult.outputPath).size(), static_cast<size_t>(4));
    EXPECT_EQ(legacyResult.assetCount, canonicalResult.assetCount);
    EXPECT_EQ(legacyResult.dllCount, canonicalResult.dllCount);
    EXPECT_TRUE(legacyResult.warnings == canonicalResult.warnings);
    EXPECT_TRUE(legacyResult.errors == canonicalResult.errors);
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(legacyResult.outputPath) / "manifest.txt"));
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(legacyResult.outputPath) / "Bin" / "CompatGame.pdb"));
    EXPECT_TRUE(legacy.GetSupportedPlatforms().empty());

    // Debug packages retain PDBs even when stripDebugSymbols is requested.
    legacy.Initialize();
    Spark::PackageConfig debugConfig;
    debugConfig.projectName = "CompatGame";
    debugConfig.outputDir = "DebugPackage";
    debugConfig.buildConfig = Spark::PackageBuildConfig::Debug;
    debugConfig.stripDebugSymbols = true;
    const auto debugResult = legacy.Package(debugConfig);
    EXPECT_TRUE(debugResult.success);
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(debugResult.outputPath) / "Bin" / "CompatGame.pdb"));
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(debugResult.outputPath) / "Bin" / "CompatGame.exe"));
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(debugResult.outputPath) / "Bin" / "Renderer.dll"));
    EXPECT_EQ(Spark::Build::GamePackager::GetInstance().GetLastResult().filesCopied, static_cast<uint32_t>(4));
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(debugResult.outputPath) / "Bin" / "Editor.dll"));
    legacy.Shutdown();

    // Missing assets are a warning, not a packaging failure.
    std::filesystem::remove_all(tmpDir / "Assets");
    legacy.Initialize();
    Spark::PackageConfig missingAssetsConfig;
    missingAssetsConfig.projectName = "CompatGame";
    missingAssetsConfig.outputDir = "MissingAssetsPackage";
    const auto missingAssetsResult = legacy.Package(missingAssetsConfig);
    EXPECT_TRUE(missingAssetsResult.success);
    EXPECT_TRUE(missingAssetsResult.errors.empty());
    EXPECT_TRUE(std::find(missingAssetsResult.warnings.begin(), missingAssetsResult.warnings.end(),
                          "Assets directory not found; skipping asset cooking") != missingAssetsResult.warnings.end());
    EXPECT_TRUE(std::find(missingAssetsResult.warnings.begin(), missingAssetsResult.warnings.end(),
                          "No assets to compress") != missingAssetsResult.warnings.end());
    EXPECT_TRUE(legacy.Console_GetStatus().find("Last package:") != std::string::npos);
    legacy.Shutdown();

    CleanupDir(tmpDir.string());
}

TEST(GamePackager_LegacyCopyFailuresAggregateAndPreserveCounts)
{
    const auto tmpDir = std::filesystem::temp_directory_path() / "spark_test_packager_copy_failure";
    CleanupDir(tmpDir.string());
    CreateTempFile((tmpDir / "build" / "Release").string(), "Blocked.dll", "binary");
    CreateTempFile((tmpDir / "build" / "Release").string(), "BlockedAgain.dll", "binary");
    CreateTempFile((tmpDir / "Assets").string(), "must_not_copy.txt", "asset");

    ScopedCurrentPath cwd(tmpDir);
    const auto outputRoot = tmpDir / "Package" / "CopyFailure_Windows_Release";
    const auto destination = outputRoot / "Bin" / "Blocked.dll";
    std::filesystem::create_directories(destination);
    std::filesystem::create_directories(outputRoot / "Bin" / "BlockedAgain.dll");

    auto& packager = Spark::GamePackager::GetInstance();
    packager.Initialize();
    Spark::PackageConfig config;
    config.projectName = "CopyFailure";
    config.outputDir = "Package";
    const auto result = packager.Package(config);
    packager.Shutdown();

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.outputPath.empty());
    EXPECT_EQ(result.assetCount, static_cast<uint32_t>(1));
    EXPECT_EQ(result.dllCount, static_cast<uint32_t>(0));
    EXPECT_TRUE(result.warnings.empty());
    EXPECT_EQ(result.errors.size(), static_cast<size_t>(2));
    EXPECT_TRUE(std::any_of(result.errors.begin(), result.errors.end(), [](const auto& error)
                            { return error.starts_with("Failed to copy binary 'Blocked.dll': "); }));
    EXPECT_TRUE(std::any_of(result.errors.begin(), result.errors.end(), [](const auto& error)
                            { return error.starts_with("Failed to copy binary 'BlockedAgain.dll': "); }));
    EXPECT_TRUE(std::filesystem::exists(outputRoot / "Assets" / "must_not_copy.txt"));
    EXPECT_FALSE(std::filesystem::exists(outputRoot / "manifest.txt"));
    EXPECT_EQ(Spark::Build::GamePackager::GetInstance().GetLastResult().filesCopied, static_cast<uint32_t>(1));

    CleanupDir(tmpDir.string());
}
