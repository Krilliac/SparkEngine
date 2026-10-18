/**
 * @file TestENG200VisualScriptRuntimeReal.cpp
 * @brief ENG-200: visual-script graphs compiled into a package play to the win through the packaged-runtime path.
 *
 * The chain under test, with no step taken from the source tree's generated scripts:
 *
 * 1. Every GameModules/SparkGameVisualScript/Assets/Graphs/<Class>.vscript is loaded
 *    with the engine's VisualScriptGraphIO and compiled with VisualScriptCompiler.
 * 2. Spark::Build::GamePackager lays the compiled scripts out as a package
 *    (<package>/Assets/Scripts/Generated/<Class>.as) beside the spark-cli package
 *    markers manifest.json and spark.modules.json and a stand-in executable file.
 * 3. Launched from an unrelated working directory, RuntimePackage::AnchorWorkingDirectory
 *    recognizes the package and anchors to it, and the module's own
 *    VisualScriptDemo::ScriptSearchPaths (the list SparkGameVisualScriptModule::OnLoad
 *    loads from) resolves the package's script root.
 * 4. The module's DemoWorld validates and spawns those scripts, and the shared
 *    headless harness plays them to the five-pickup win.
 *
 * The stand-in executable is a placeholder file: this proves the package layout,
 * root resolution and gameplay in-process, not a launch of the packaged engine
 * binary, which the hosted visual-script package job owns.
 */

#include "TestFramework.h"

#ifdef SPARK_ANGELSCRIPT_SUPPORT

#include "VisualScriptGameplayHarness.h"
#include "Core/RuntimePackage.h"
#include "Engine/Build/GamePackager.h"
#include "Engine/Scripting/VisualScriptCompiler.h"
#include "Engine/Scripting/VisualScriptGraphIO.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using Spark::Build::GamePackager;
    using Spark::Build::PackageConfig;
    using Spark::Build::PackagePlatform;
    using Spark::RuntimePackage::AnchorWorkingDirectory;
    using Spark::RuntimePackage::WorkingDirectoryResult;
    using Spark::VisualScriptDemo::ScriptManifest;
    using Spark::VisualScriptDemo::ScriptSearchPaths;
    using VisualScriptGameplayHarness::GameplayFixture;

    constexpr std::string_view kProjectName = "VisualScriptGame";
    constexpr int kFrameBudget = 60 * 60; // one minute of game time

    const fs::path kModuleGraphs = fs::path(SPARK_TEST_SOURCE_DIR) / "GameModules/SparkGameVisualScript/Assets/Graphs";

    /// A unique scratch directory, removed on every exit path.
    struct ScratchDirectory
    {
        fs::path path;

        ScratchDirectory()
        {
            static std::atomic<uint32_t> sequence{0};
            path = fs::temp_directory_path() /
                   ("spark_eng200_vsruntime_" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                    std::to_string(sequence++));
            fs::create_directories(path);
        }
        ~ScratchDirectory()
        {
            std::error_code error;
            fs::remove_all(path, error);
        }
        ScratchDirectory(const ScratchDirectory&) = delete;
        ScratchDirectory& operator=(const ScratchDirectory&) = delete;
    };

    /// Restores the process working directory on every exit path (AnchorWorkingDirectory changes it).
    struct ScopedWorkingDirectory
    {
        fs::path saved = fs::current_path();

        explicit ScopedWorkingDirectory(const fs::path& directory) { fs::current_path(directory); }
        ~ScopedWorkingDirectory()
        {
            std::error_code error;
            fs::current_path(saved, error);
        }
        ScopedWorkingDirectory(const ScopedWorkingDirectory&) = delete;
        ScopedWorkingDirectory& operator=(const ScopedWorkingDirectory&) = delete;
    };

    PackagePlatform HostPlatform()
    {
#if defined(_WIN32)
        return PackagePlatform::WindowsX64;
#elif defined(__APPLE__) && defined(__aarch64__)
        return PackagePlatform::MacOSARM64;
#elif defined(__APPLE__)
        return PackagePlatform::MacOSX64;
#else
        return PackagePlatform::LinuxX64;
#endif
    }

    bool WriteText(const fs::path& path, std::string_view text)
    {
        fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        stream << text;
        return static_cast<bool>(stream);
    }

    /**
     * Compile the five graphs in @p graphDirectory and package the result under @p work / "out".
     * @return The package root, or nullopt with @p diagnostic naming the graph or packaging failure.
     *         A graph that fails to load or compile stops the build before anything is packaged.
     */
    std::optional<fs::path> PackageCompiledGraphs(const fs::path& work, const fs::path& graphDirectory,
                                                  std::string& diagnostic)
    {
        const fs::path stage = work / "stage";
        const fs::path stagedScripts = stage / "Assets" / "Scripts" / "Generated";
        for (const auto& asset : ScriptManifest)
        {
            const fs::path graphPath = graphDirectory / (std::string(asset.className) + ".vscript");
            const auto graph = Spark::Scripting::VisualScriptGraphIO::LoadFile(graphPath);
            if (!graph)
            {
                diagnostic = graph.error();
                return std::nullopt;
            }
            const auto compiled = Spark::Scripting::VisualScriptCompiler::Compile(*graph);
            if (!compiled.success)
            {
                diagnostic = graphPath.generic_string() + ": does not compile";
                for (const auto& error : compiled.errors)
                    diagnostic += "; " + error;
                return std::nullopt;
            }
            if (!WriteText(stagedScripts / fs::path(asset.fileName), compiled.angelScriptSource))
            {
                diagnostic = "cannot stage " + (stagedScripts / fs::path(asset.fileName)).generic_string();
                return std::nullopt;
            }
        }

        // The spark-cli package markers AnchorWorkingDirectory recognizes, and the executable the package is built
        // around (a placeholder: this test does not launch it).
        const PackagePlatform platform = HostPlatform();
        const fs::path executable = stage / ("SparkEngine" + GamePackager::GetExecutableExtension(platform));
        const fs::path packageManifest = stage / "manifest.json";
        const fs::path moduleManifest = stage / "spark.modules.json";
        if (!WriteText(executable, "stand-in executable\n") ||
            !WriteText(packageManifest, "{\n  \"name\": \"VisualScriptGame\",\n  \"modules\": "
                                        "[\"SparkGameVisualScript\"]\n}\n") ||
            !WriteText(moduleManifest, "{\n  \"modules\": [{\"name\": \"SparkGameVisualScript\"}]\n}\n"))
        {
            diagnostic = "cannot stage the package executable and manifests under " + stage.generic_string();
            return std::nullopt;
        }

        PackageConfig config;
        config.projectName = std::string(kProjectName);
        config.executablePath = executable.string();
        config.outputDirectory = (work / "out").string();
        config.assetDirectory = (stage / "Assets").string();
        config.dataDirectory.clear();
        config.platform = platform;
        config.extraFiles = {packageManifest.string(), moduleManifest.string()};

        auto& packager = GamePackager::GetInstance();
        packager.Initialize();
        const auto result = packager.Package(config);
        packager.Shutdown();
        if (!result.success)
        {
            diagnostic = "GamePackager failed: " + result.errorMessage;
            return std::nullopt;
        }
        return fs::path(result.outputPath);
    }

    bool IsUnder(const fs::path& path, const fs::path& root)
    {
        const auto relative = fs::weakly_canonical(path).lexically_relative(fs::weakly_canonical(root));
        return !relative.empty() && *relative.begin() != fs::path("..");
    }

    size_t CountDemoEntities(GameplayFixture& fx)
    {
        size_t count = 0;
        for (auto entity : fx.world.GetEntitiesWith<NameComponent>())
        {
            if (fx.world.GetComponent<NameComponent>(entity)->name.starts_with("VS"))
                ++count;
        }
        return count;
    }
} // namespace

TEST(VisualScriptRuntime_GraphsCompiledIntoPackagePlayToWin)
{
    ScratchDirectory scratch;
    std::string diagnostic;
    const auto package = PackageCompiledGraphs(scratch.path, kModuleGraphs, diagnostic);
    if (!package)
        std::printf("  %s\n", diagnostic.c_str());
    ASSERT_TRUE(package.has_value());

    // The packager's layout: the executable and both markers at the root, the compiled scripts under Assets.
    EXPECT_EQ(package->filename().string(), std::string(kProjectName));
    EXPECT_TRUE(fs::is_regular_file(*package / "manifest.json"));
    EXPECT_TRUE(fs::is_regular_file(*package / "spark.modules.json"));
    for (const auto& asset : ScriptManifest)
        EXPECT_TRUE(fs::is_regular_file(*package / "Assets/Scripts/Generated" / fs::path(asset.fileName)));

    // Launched from an unrelated directory, the runtime anchors to the package and loads its scripts.
    const fs::path elsewhere = scratch.path / "elsewhere";
    fs::create_directories(elsewhere);
    ScopedWorkingDirectory launchedFrom(elsewhere);
    std::error_code anchorError;
    ASSERT_TRUE(AnchorWorkingDirectory(*package, anchorError) == WorkingDirectoryResult::Anchored);
    EXPECT_TRUE(fs::equivalent(fs::current_path(), *package));

    const auto searchPaths = ScriptSearchPaths(*package, fs::current_path());
    GameplayFixture fx(searchPaths);
    if (!fx.ready)
        std::printf("  %s\n", fx.demo->GetLastError().c_str());
    ASSERT_TRUE(fx.ready);
    EXPECT_TRUE(fs::equivalent(fx.demo->GetScriptRoot(), *package / "Assets/Scripts/Generated"));

    // Five pickups, 500 points and exactly one win, decided by the graph-compiled scripts.
    const int frames = fx.CollectAllCoins(kFrameBudget);
    ASSERT_TRUE(frames < kFrameBudget);
    fx.ReleaseAllKeys();
    fx.Tick(); // GameManager evaluates the win on its next Update()
    for (int i = 0; i < 5; ++i)
        EXPECT_TRUE(fx.Position(fx.Find("VS_Coin_" + std::to_string(i))).y == -100.0f);
    EXPECT_EQ(fx.CountOutput("Collected! +100 points"), static_cast<size_t>(5));
    EXPECT_EQ(fx.Health(fx.Find("VS_GameManager")), 500.0f);
    EXPECT_EQ(fx.CountOutput("*** YOU WIN! ***"), static_cast<size_t>(1));
    EXPECT_EQ(fx.CountOutput("*** GAME OVER ***"), static_cast<size_t>(0));
    EXPECT_FALSE(fx.AnyScriptFaulted());
}

TEST(VisualScriptRuntime_PackagedRootWinsFromForeignWorkingDirectory)
{
    ScratchDirectory scratch;
    std::string diagnostic;
    const auto package = PackageCompiledGraphs(scratch.path, kModuleGraphs, diagnostic);
    if (!package)
        std::printf("  %s\n", diagnostic.c_str());
    ASSERT_TRUE(package.has_value());

    // The launch directory holds a complete decoy script set whose PlayerController does not compile.
    const fs::path foreign = scratch.path / "foreign";
    const fs::path decoy = foreign / "Assets/Scripts/Generated";
    fs::create_directories(decoy);
    for (const auto& asset : ScriptManifest)
        fs::copy_file(*package / "Assets/Scripts/Generated" / fs::path(asset.fileName),
                      decoy / fs::path(asset.fileName));
    ASSERT_TRUE(WriteText(decoy / "PlayerController.as", "class PlayerController { this does not compile }\n"));
    {
        // Loading the decoy is a rejected load, so a runtime that picked it could not reach the gameplay below.
        GameplayFixture decoyOnly(std::array<fs::path, 1>{decoy});
        EXPECT_FALSE(decoyOnly.ready);
        EXPECT_STR_CONTAINS(decoyOnly.demo->GetLastError(), (decoy / "PlayerController.as").generic_string());
    }

    ScopedWorkingDirectory launchedFrom(foreign);
    std::error_code anchorError;
    ASSERT_TRUE(AnchorWorkingDirectory(*package, anchorError) == WorkingDirectoryResult::Anchored);
    const auto searchPaths = ScriptSearchPaths(*package, fs::current_path());
    for (const auto& candidate : searchPaths)
        EXPECT_FALSE(IsUnder(candidate, foreign));

    GameplayFixture fx(searchPaths);
    if (!fx.ready)
        std::printf("  %s\n", fx.demo->GetLastError().c_str());
    ASSERT_TRUE(fx.ready);
    EXPECT_TRUE(IsUnder(fx.demo->GetScriptRoot(), *package));
    EXPECT_EQ(CountDemoEntities(fx), static_cast<size_t>(Spark::VisualScriptDemo::ExpectedEntityCount + 5));

    // The packaged PlayerController drives the player: holding W moves it forward.
    const float startZ = fx.Position(fx.Find("VS_Player")).z;
    for (int i = 0; i < 30; ++i)
    {
        fx.SetKey('W', true);
        fx.Tick();
    }
    fx.SetKey('W', false);
    EXPECT_GT(fx.Position(fx.Find("VS_Player")).z, startZ + 1.0f);
    EXPECT_FALSE(fx.AnyScriptFaulted());
}

TEST(VisualScriptRuntime_BrokenGraphInPackageRejectsLoadWithDiagnostic)
{
    ScratchDirectory scratch;

    // A copy of the module's graphs with EnemyPatrol.vscript cut off halfway.
    const fs::path graphs = scratch.path / "graphs";
    fs::create_directories(graphs);
    for (const auto& asset : ScriptManifest)
    {
        const std::string graphFile = std::string(asset.className) + ".vscript";
        fs::copy_file(kModuleGraphs / graphFile, graphs / graphFile);
    }
    const fs::path broken = graphs / "EnemyPatrol.vscript";
    const auto size = fs::file_size(broken);
    ASSERT_TRUE(size > 2);
    fs::resize_file(broken, size / 2);

    // The build stops at the broken graph, names it, and packages nothing.
    std::string diagnostic;
    const auto package = PackageCompiledGraphs(scratch.path, graphs, diagnostic);
    EXPECT_FALSE(package.has_value());
    EXPECT_STR_CONTAINS(diagnostic, broken.generic_string());
    const fs::path wouldBePackage = scratch.path / "out" / std::string(kProjectName);
    EXPECT_FALSE(fs::exists(wouldBePackage));

    // A runtime pointed at the missing package is not anchored, rejects the load naming the root it searched,
    // and leaves no demo entity behind.
    const fs::path elsewhere = scratch.path / "elsewhere";
    fs::create_directories(elsewhere);
    ScopedWorkingDirectory launchedFrom(elsewhere);
    std::error_code anchorError;
    EXPECT_TRUE(AnchorWorkingDirectory(wouldBePackage, anchorError) == WorkingDirectoryResult::NotPackaged);
    GameplayFixture fx(ScriptSearchPaths(wouldBePackage, fs::current_path()));
    EXPECT_FALSE(fx.ready);
    EXPECT_STR_CONTAINS(fx.demo->GetLastError(),
                        (wouldBePackage / "Assets/Scripts/Generated").lexically_normal().generic_string());
    EXPECT_TRUE(fx.demo->GetEntities().empty());
    EXPECT_EQ(CountDemoEntities(fx), static_cast<size_t>(0));
}

#endif // SPARK_ANGELSCRIPT_SUPPORT
