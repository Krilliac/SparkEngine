/** @file TestLauncherProcess.cpp @brief SparkLauncher project-action command contract tests. */
#include "TestFramework.h"
#include "../SparkLauncher/src/LauncherProcess.h"
#include "Utils/JsonUtils.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>

namespace
{
    std::filesystem::path MakeLauncherTestRoot()
    {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        const auto root = std::filesystem::temp_directory_path() / ("spark-launcher-" + std::to_string(stamp));
        std::filesystem::create_directories(root / "bin");
        std::filesystem::create_directories(root / "project" / "Config");
        return root;
    }

    std::filesystem::path Executable(const std::filesystem::path& directory, const char* name)
    {
#ifdef _WIN32
        return directory / (std::string(name) + ".exe");
#else
        return directory / name;
#endif
    }

    void Touch(const std::filesystem::path& path)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << "fixture";
    }

    std::filesystem::path NativeModule(const std::filesystem::path& directory, const char* name)
    {
#ifdef _WIN32
        return directory / (std::string(name) + ".dll");
#elif defined(__APPLE__)
        return directory / ("lib" + std::string(name) + ".dylib");
#else
        return directory / ("lib" + std::string(name) + ".so");
#endif
    }

    std::filesystem::path AbiSidecar(std::filesystem::path module)
    {
        module += ".sparkabi";
        return module;
    }

    void WriteModuleManifest(const std::filesystem::path& projectRoot, const std::vector<std::string>& declaredPaths)
    {
        Spark::Json::Value root = Spark::Json::Value::MakeObject();
        Spark::Json::Value modules = Spark::Json::Value::MakeArray();
        for (size_t index = 0; index < declaredPaths.size(); ++index)
        {
            Spark::Json::Value entry = Spark::Json::Value::MakeObject();
            entry["name"] = Spark::Json::Value("Module" + std::to_string(index));
            entry["path"] = Spark::Json::Value(declaredPaths[index]);
            entry["loadOrder"] = Spark::Json::Value(static_cast<int>(1000 + index));
            modules.PushBack(std::move(entry));
        }
        root["modules"] = std::move(modules);
        std::ofstream(projectRoot / "spark.modules.json") << Spark::Json::StringifyPretty(root) << '\n';
    }

    Spark::Json::Value ReadJson(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        Spark::Json::Value value;
        std::string error;
        EXPECT_TRUE(Spark::Json::ParseStrict(content, &value, &error));
        return value;
    }
} // namespace

TEST(LauncherProcess_BuildsEditorGameAndServiceTopologyRequests)
{
    using namespace SparkLauncher;
    const auto root = MakeLauncherTestRoot();
    const auto binaries = root / "bin";
    const auto projectRoot = root / "project" / std::filesystem::u8path("Caf\xC3\xA9 Project");
    const auto project = projectRoot / "Sample.sparkproject";
    Touch(project);
    Touch(Executable(binaries, "SparkEditor"));
    Touch(Executable(binaries, "SparkEngine"));
    WriteModuleManifest(projectRoot, {"Modules\\Sample.dll", "SampleAddon.dll"});
    const auto gameModule = NativeModule(projectRoot / "build" / "Release", "Sample");
    const auto addonModule = NativeModule(projectRoot / "build" / "Release", "SampleAddon");
    Touch(gameModule);
    Touch(AbiSidecar(gameModule));
    Touch(addonModule);
    Touch(AbiSidecar(addonModule));

    auto editor = BuildLaunchRequest(binaries, project, LaunchTarget::Editor);
    EXPECT_TRUE(editor.has_value());
    EXPECT_EQ(editor->workingDirectory, project.parent_path());
    EXPECT_EQ(editor->arguments.size(), static_cast<size_t>(2));
    EXPECT_EQ(editor->arguments[0], std::string("--project"));

    auto game = BuildLaunchRequest(binaries, project, LaunchTarget::Game);
    EXPECT_TRUE(game.has_value());
    EXPECT_EQ(game->executable, Executable(binaries, "SparkEngine"));
    EXPECT_EQ(game->arguments.size(), static_cast<size_t>(4));
    EXPECT_EQ(game->arguments[0], std::string("-manifest"));
    EXPECT_EQ(std::filesystem::u8path(game->arguments[1]),
              std::filesystem::weakly_canonical(projectRoot / "build" / ".spark-launcher" / "spark.modules.json"));
    EXPECT_EQ(game->arguments[2], std::string("--project"));
    EXPECT_EQ(std::filesystem::u8path(game->arguments[3]), std::filesystem::weakly_canonical(project));

    const auto resolvedManifest = ReadJson(std::filesystem::u8path(game->arguments[1]));
    EXPECT_EQ(resolvedManifest["modules"].Size(), static_cast<size_t>(2));
    EXPECT_EQ(std::filesystem::u8path(resolvedManifest["modules"][static_cast<size_t>(0)]["path"].AsString()),
              std::filesystem::weakly_canonical(gameModule));
    EXPECT_EQ(std::filesystem::u8path(resolvedManifest["modules"][static_cast<size_t>(1)]["path"].AsString()),
              std::filesystem::weakly_canonical(addonModule));
    EXPECT_EQ(resolvedManifest["modules"][static_cast<size_t>(1)]["loadOrder"].AsInt(), 1001);

    auto services = BuildLaunchRequest(binaries, project, LaunchTarget::ServiceTopology);
    EXPECT_TRUE(services.has_value());
    EXPECT_EQ(services->arguments.size(), static_cast<size_t>(4));
    EXPECT_EQ(services->arguments[2], std::string("--open-panel"));
    EXPECT_EQ(services->arguments[3], std::string("ServiceTopology"));

    std::error_code error;
    std::filesystem::remove_all(root, error);
    EXPECT_FALSE(error);
}

TEST(LauncherProcess_GameLaunchFailsClosedForMissingInvalidAndAmbiguousModules)
{
    using namespace SparkLauncher;
    const auto root = MakeLauncherTestRoot();
    const auto binaries = root / "bin";
    const auto project = root / "project" / "Sample.sparkproject";
    Touch(project);
    Touch(Executable(binaries, "SparkEngine"));

    auto missingManifest = BuildLaunchRequest(binaries, project, LaunchTarget::Game);
    EXPECT_FALSE(missingManifest.has_value());
    EXPECT_TRUE(missingManifest.error().find("manifest not found") != std::string::npos);

    WriteModuleManifest(project.parent_path(), {"../Outside.dll"});
    auto escapingPath = BuildLaunchRequest(binaries, project, LaunchTarget::Game);
    EXPECT_FALSE(escapingPath.has_value());
    EXPECT_TRUE(escapingPath.error().find("must not escape") != std::string::npos);

#ifndef _WIN32
    const auto outsideModule = NativeModule(root / "outside", "Outside");
    const auto linkedModule = project.parent_path() / outsideModule.filename();
    Touch(outsideModule);
    Touch(AbiSidecar(outsideModule));
    std::error_code linkError;
    std::filesystem::create_symlink(outsideModule, linkedModule, linkError);
    EXPECT_FALSE(linkError);
    Touch(AbiSidecar(linkedModule));
    WriteModuleManifest(project.parent_path(), {linkedModule.filename().string()});
    auto escapingSymlink = BuildLaunchRequest(binaries, project, LaunchTarget::Game);
    EXPECT_FALSE(escapingSymlink.has_value());
    EXPECT_TRUE(escapingSymlink.error().find("symlink escapes") != std::string::npos);
    std::filesystem::remove(linkedModule, linkError);
    std::filesystem::remove(AbiSidecar(linkedModule), linkError);
#endif

    WriteModuleManifest(project.parent_path(), {"Sample.dll"});
    const auto debugModule = NativeModule(project.parent_path() / "build" / "Debug", "Sample");
    const auto releaseModule = NativeModule(project.parent_path() / "build" / "Release", "Sample");
    Touch(debugModule);
    Touch(AbiSidecar(debugModule));
    Touch(releaseModule);
    Touch(AbiSidecar(releaseModule));
    auto ambiguous = BuildLaunchRequest(binaries, project, LaunchTarget::Game);
    EXPECT_FALSE(ambiguous.has_value());
    EXPECT_TRUE(ambiguous.error().find("Multiple built modules") != std::string::npos);

    std::error_code error;
    std::filesystem::remove_all(root, error);
    EXPECT_FALSE(error);
}

TEST(LauncherProcess_GameLaunchUsesSelfContainedPackageContext)
{
    using namespace SparkLauncher;
    const auto root = MakeLauncherTestRoot();
    const auto binaries = root / "bin";
    const auto project = root / "project" / "Sample.sparkproject";
    const auto package = project.parent_path() / "Build" / "Output";
    Touch(project);
    Touch(Executable(binaries, "SparkEngine"));
    WriteModuleManifest(project.parent_path(), {"Sample.dll"});

    Touch(package / "manifest.json");
    Touch(package / project.filename());
    Touch(Executable(package, "SparkEngine"));
    WriteModuleManifest(package, {NativeModule({}, "Sample").filename().string()});
    const auto module = NativeModule(package, "Sample");
    Touch(module);
    Touch(AbiSidecar(module));

    auto game = BuildLaunchRequest(binaries, project, LaunchTarget::Game);
    EXPECT_TRUE(game.has_value());
    EXPECT_EQ(game->executable, std::filesystem::weakly_canonical(Executable(package, "SparkEngine")));
    EXPECT_EQ(game->workingDirectory, std::filesystem::weakly_canonical(package));
    EXPECT_EQ(game->arguments.size(), static_cast<size_t>(4));
    EXPECT_EQ(game->arguments[0], std::string("-manifest"));
    EXPECT_EQ(std::filesystem::u8path(game->arguments[1]),
              std::filesystem::weakly_canonical(package / "spark.modules.json"));
    EXPECT_EQ(game->arguments[2], std::string("--project"));
    EXPECT_EQ(std::filesystem::u8path(game->arguments[3]),
              std::filesystem::weakly_canonical(package / project.filename()));

    const auto packageManifest = ReadJson(std::filesystem::u8path(game->arguments[1]));
    EXPECT_EQ(packageManifest["modules"].Size(), static_cast<size_t>(1));
    const auto declaredModule =
        std::filesystem::u8path(packageManifest["modules"][static_cast<size_t>(0)]["path"].AsString());
    EXPECT_TRUE(declaredModule.is_relative());
    EXPECT_EQ(std::filesystem::weakly_canonical(package / declaredModule), std::filesystem::weakly_canonical(module));

    std::error_code error;
    std::filesystem::remove_all(root, error);
    EXPECT_FALSE(error);
}

TEST(LauncherProcess_RequiresServerConfigAndExecutable)
{
    using namespace SparkLauncher;
    const auto root = MakeLauncherTestRoot();
    const auto binaries = root / "bin";
    const auto project = root / "project" / "Sample.sparkproject";
    Touch(project);
    Touch(Executable(binaries, "SparkServer"));

    auto missingConfig = BuildLaunchRequest(binaries, project, LaunchTarget::DedicatedServer);
    EXPECT_FALSE(missingConfig.has_value());
    EXPECT_TRUE(missingConfig.error().find("server.ini") != std::string::npos);

    const auto config = root / "project" / "Config" / "server.ini";
    Touch(config);
    auto server = BuildLaunchRequest(binaries, project, LaunchTarget::DedicatedServer);
    EXPECT_TRUE(server.has_value());
    EXPECT_EQ(server->arguments.size(), static_cast<size_t>(2));
    EXPECT_EQ(server->arguments[0], std::string("--config"));
    EXPECT_EQ(server->arguments[1], config.string());

    std::error_code error;
    std::filesystem::remove_all(root, error);
    EXPECT_FALSE(error);
}
