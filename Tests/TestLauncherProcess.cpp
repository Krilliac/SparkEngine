/** @file TestLauncherProcess.cpp @brief SparkLauncher project-action command contract tests. */
#include "TestFilesystemLinks.h"
#include "TestFramework.h"
#include "../SparkLauncher/src/LauncherProcess.h"
#include "../SparkLauncher/src/LauncherTemplates.h"
#include "Utils/JsonUtils.h"

#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

#ifndef _WIN32
#include <sys/stat.h>
#endif

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

    // Directory links run on every host: an NTFS junction on Windows (no privilege
    // needed, and not reported as a symlink), a symlink elsewhere. A module reached
    // through one that leads out of the project is refused.
    const auto linkedModuleDirectory = project.parent_path() / "LinkedModules";
    const auto outsideDirectoryModule = NativeModule(root / "outside-dir", "Linked");
    Touch(outsideDirectoryModule);
    Touch(AbiSidecar(outsideDirectoryModule));
    ASSERT_TRUE(SparkTestLinks::MakeDirectoryLink(outsideDirectoryModule.parent_path(), linkedModuleDirectory));
    WriteModuleManifest(project.parent_path(), {"LinkedModules/" + outsideDirectoryModule.filename().string()});
    auto escapingDirectoryLink = BuildLaunchRequest(binaries, project, LaunchTarget::Game);
    EXPECT_FALSE(escapingDirectoryLink.has_value());
    if (!escapingDirectoryLink)
        EXPECT_TRUE(escapingDirectoryLink.error().find("symlink escapes") != std::string::npos);
    EXPECT_TRUE(SparkTestLinks::RemoveDirectoryLink(linkedModuleDirectory));

    // So is a project build directory that is a link out of the project.
    const auto buildLink = project.parent_path() / "build";
    std::filesystem::create_directories(root / "outside-build");
    ASSERT_TRUE(SparkTestLinks::MakeDirectoryLink(root / "outside-build", buildLink));
    WriteModuleManifest(project.parent_path(), {"Sample.dll"});
    auto escapingBuild = BuildLaunchRequest(binaries, project, LaunchTarget::Game);
    EXPECT_FALSE(escapingBuild.has_value());
    if (!escapingBuild)
        EXPECT_TRUE(escapingBuild.error().find("build directory escapes") != std::string::npos);
    EXPECT_TRUE(SparkTestLinks::RemoveDirectoryLink(buildLink));

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

TEST(LauncherProcess_GameLaunchRefusesOversizedModuleManifest)
{
    using namespace SparkLauncher;
    const auto root = MakeLauncherTestRoot();
    const auto binaries = root / "bin";
    const auto projectRoot = root / "project";
    const auto project = projectRoot / "Sample.sparkproject";
    Touch(project);
    Touch(Executable(binaries, "SparkEngine"));
    const auto module = NativeModule(projectRoot / "build" / "Release", "Sample");
    Touch(module);
    Touch(AbiSidecar(module));

    // A valid manifest padded with trailing whitespace past the cap. It is well
    // formed JSON, so only the size bound can refuse it.
    WriteModuleManifest(projectRoot, {"Sample.dll"});
    auto accepted = BuildLaunchRequest(binaries, project, LaunchTarget::Game);
    EXPECT_TRUE(accepted.has_value());
    {
        std::ofstream padding(projectRoot / "spark.modules.json", std::ios::binary | std::ios::app);
        const std::string spaces(kMaxModuleManifestBytes, ' ');
        padding << spaces;
    }
    std::error_code sizeError;
    EXPECT_TRUE(std::filesystem::file_size(projectRoot / "spark.modules.json", sizeError) > kMaxModuleManifestBytes);

    auto oversized = BuildLaunchRequest(binaries, project, LaunchTarget::Game);
    EXPECT_FALSE(oversized.has_value());
    if (!oversized.has_value())
    {
        EXPECT_TRUE(oversized.error().find("exceeds the") != std::string::npos);
    }

    // Exactly at the cap is still accepted: the bound is inclusive.
    {
        std::ofstream exact(projectRoot / "spark.modules.json", std::ios::binary | std::ios::trunc);
        const std::string body = "{\"modules\":[{\"path\":\"Sample.dll\"}]}";
        exact << body << std::string(kMaxModuleManifestBytes - body.size(), ' ');
    }
    auto atLimit = BuildLaunchRequest(binaries, project, LaunchTarget::Game);
    EXPECT_TRUE(atLimit.has_value());

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
    EXPECT_EQ(std::filesystem::u8path(server->arguments[1]), config);

    std::error_code error;
    std::filesystem::remove_all(root, error);
    EXPECT_FALSE(error);
}

// SEC4: every LaunchRequest argument is UTF-8. The Editor, DedicatedServer and
// ServiceTopology targets used path::string(), which on Windows is the active code
// page: LaunchDetached then threw std::system_error decoding it (for an accented
// folder), or string() itself threw (for characters outside the code page), and
// nothing in the launcher UI caught either. U+0915 exists in no ANSI code page, so
// the round trip below fails on Windows without the fix whatever the host's ACP is.
TEST(SEC4Launcher_EditorServerAndServiceArgumentsAreUtf8ForNonAsciiProjects)
{
    using namespace SparkLauncher;
    const auto root = MakeLauncherTestRoot();
    const auto binaries = root / "bin";
    auto projectRoot = PathFromUtf8("Caf\xC3\xA9 \xE0\xA4\x95 Project");
    EXPECT_TRUE(projectRoot.has_value());
    if (!projectRoot)
        return;
    const auto project = root / *projectRoot / "Sample.sparkproject";
    const auto config = project.parent_path() / "Config" / "server.ini";
    Touch(project);
    Touch(config);
    Touch(Executable(binaries, "SparkEditor"));
    Touch(Executable(binaries, "SparkServer"));

    const auto decodedArgument = [](const std::string& argument)
    {
        auto decoded = PathFromUtf8(argument);
        EXPECT_TRUE(decoded.has_value());
        return decoded.value_or(std::filesystem::path{});
    };

    auto editor = BuildLaunchRequest(binaries, project, LaunchTarget::Editor);
    EXPECT_TRUE(editor.has_value());
    if (editor)
    {
        EXPECT_EQ(editor->arguments.size(), static_cast<size_t>(2));
        EXPECT_EQ(decodedArgument(editor->arguments[1]), project);
    }

    auto services = BuildLaunchRequest(binaries, project, LaunchTarget::ServiceTopology);
    EXPECT_TRUE(services.has_value());
    if (services)
    {
        EXPECT_EQ(services->arguments.size(), static_cast<size_t>(4));
        EXPECT_EQ(decodedArgument(services->arguments[1]), project);
    }

    auto server = BuildLaunchRequest(binaries, project, LaunchTarget::DedicatedServer);
    EXPECT_TRUE(server.has_value());
    if (server)
    {
        EXPECT_EQ(server->arguments.size(), static_cast<size_t>(2));
        EXPECT_EQ(decodedArgument(server->arguments[1]), config);
    }

    // Error messages carry the path as UTF-8 too instead of throwing from string().
    std::error_code removeError;
    std::filesystem::remove(config, removeError);
    auto missingConfig = BuildLaunchRequest(binaries, project, LaunchTarget::DedicatedServer);
    EXPECT_FALSE(missingConfig.has_value());
    if (!missingConfig)
    {
        EXPECT_TRUE(missingConfig.error().find("Caf\xC3\xA9 \xE0\xA4\x95 Project") != std::string::npos);
    }

    std::error_code error;
    std::filesystem::remove_all(root, error);
    EXPECT_FALSE(error);
}

// SEC4: text that is not a path fails closed as a value, never as an exception.
// LaunchDetached used std::filesystem::u8path per argument, which throws on invalid
// UTF-8; it now refuses the request before starting anything.
TEST(SEC4Launcher_InvalidUtf8AndEmbeddedNulFailClosedWithoutThrowing)
{
    using namespace SparkLauncher;
    const std::string nul("Sample\0.sparkproject", 20);
    EXPECT_FALSE(PathFromUtf8(nul).has_value());

    const auto roundTrip = PathFromUtf8(PathToUtf8(std::filesystem::path(u8"Caf\u00e9/\u0915.sparkproject")));
    EXPECT_TRUE(roundTrip.has_value());
    if (roundTrip)
    {
        EXPECT_EQ(*roundTrip, std::filesystem::path(u8"Caf\u00e9/\u0915.sparkproject"));
    }

    LaunchRequest request;
    request.executable = std::filesystem::temp_directory_path() / "spark-sec4-launcher-missing-executable";
    request.workingDirectory = std::filesystem::temp_directory_path();
    request.arguments = {"--project", nul};
    auto launchedWithNul = LaunchDetached(request);
    EXPECT_FALSE(launchedWithNul.has_value());
    if (!launchedWithNul)
    {
        EXPECT_TRUE(launchedWithNul.error().find("NUL") != std::string::npos);
    }

#ifdef _WIN32
    // Windows paths are UTF-16, so malformed UTF-8 has no spelling and is refused.
    const std::string latin1Bytes = "C:\\Caf\xE9\\Sample.sparkproject";
    auto decoded = PathFromUtf8(latin1Bytes);
    EXPECT_FALSE(decoded.has_value());
    if (!decoded)
    {
        EXPECT_TRUE(decoded.error().find("UTF-8") != std::string::npos);
    }

    request.arguments = {"--project", latin1Bytes};
    bool threw = false;
    std::expected<void, std::string> launched;
    try
    {
        launched = LaunchDetached(request);
    }
    catch (...)
    {
        threw = true;
    }
    EXPECT_FALSE(threw);
    EXPECT_FALSE(launched.has_value());
    if (!launched)
    {
        EXPECT_TRUE(launched.error().find("UTF-8") != std::string::npos);
    }
#endif
}

// ============================================================================
// SEC-120 launcher-module-manifest and launcher-template-json targets
// ============================================================================

namespace
{
    void WriteBytes(const std::filesystem::path& path, const std::string& bytes)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
    }
} // namespace

TEST(LauncherProcess_BoundedReadDecidesOnTheOpenedFile)
{
    using namespace SparkLauncher;
    const auto root = MakeLauncherTestRoot();
    const auto file = root / "data.json";

    WriteBytes(file, std::string(16, 'x'));
    const auto atLimit = ReadBoundedRegularFile(file, 16);
    EXPECT_TRUE(atLimit.has_value() && *atLimit == std::string(16, 'x'));

    const auto oversized = ReadBoundedRegularFile(file, 15);
    EXPECT_FALSE(oversized.has_value());
    if (!oversized)
    {
        EXPECT_TRUE(oversized.error().find("exceeds the 15-byte limit") != std::string::npos);
    }

    const auto directory = ReadBoundedRegularFile(root / "bin", 1024);
    EXPECT_FALSE(directory.has_value());
    const auto missing = ReadBoundedRegularFile(root / "missing.json", 1024);
    EXPECT_FALSE(missing.has_value());

#ifndef _WIN32
    // Opening a FIFO by name blocked until a writer appeared; the read must refuse it at once.
    const auto fifo = root / "fifo.json";
    ASSERT_TRUE(::mkfifo(fifo.c_str(), 0600) == 0);
    const auto fromFifo = ReadBoundedRegularFile(fifo, 1024);
    EXPECT_FALSE(fromFifo.has_value());
    if (!fromFifo)
    {
        EXPECT_TRUE(fromFifo.error().find("not a regular file") != std::string::npos);
    }
#endif

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST(LauncherProcess_GameLaunchRefusesManifestPathWithEmbeddedNul)
{
    using namespace SparkLauncher;
    const auto root = MakeLauncherTestRoot();
    const auto binaries = root / "bin";
    const auto projectRoot = root / "project";
    const auto project = projectRoot / "Sample.sparkproject";
    Touch(project);
    Touch(Executable(binaries, "SparkEngine"));
    // A plain file whose name is the part before the NUL.
    Touch(projectRoot / "Game");

    // "Game\u0000.so" kept its NUL through std::filesystem::u8path, and the OS read the name
    // only up to it, so the module and its .sparkabi sidecar both resolved to the file "Game".
    // The JSON escape \u0000 decodes to that NUL, so it reaches the path check.
    const std::string nativeExtension = NativeModule(projectRoot, "x").extension().string();
    WriteBytes(projectRoot / "spark.modules.json", "{\"modules\":[{\"path\":\"Game\\u0000" + nativeExtension + "\"}]}");
    const auto request = BuildLaunchRequest(binaries, project, LaunchTarget::Game);
    EXPECT_FALSE(request.has_value());
    if (!request)
    {
        EXPECT_TRUE(request.error().find("NUL") != std::string::npos);
    }

    // A raw NUL byte never reaches the path check: unescaped control bytes are not JSON.
    std::string rawManifest = "{\"modules\":[{\"path\":\"Game";
    rawManifest.push_back('\0');
    rawManifest += nativeExtension + "\"}]}";
    WriteBytes(projectRoot / "spark.modules.json", rawManifest);
    const auto rawRequest = BuildLaunchRequest(binaries, project, LaunchTarget::Game);
    EXPECT_FALSE(rawRequest.has_value());
    if (!rawRequest)
    {
        EXPECT_TRUE(rawRequest.error().find("not valid JSON") != std::string::npos);
    }

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST(LauncherProcess_GameLaunchRefusesUndecodableManifestPathWithoutThrowing)
{
    using namespace SparkLauncher;
    const auto root = MakeLauncherTestRoot();
    const auto binaries = root / "bin";
    const auto projectRoot = root / "project";
    const auto project = projectRoot / "Sample.sparkproject";
    Touch(project);
    Touch(Executable(binaries, "SparkEngine"));

    // std::filesystem::u8path threw on these bytes on Windows, and nothing on the launch
    // path catches it. Every platform must answer with an error instead.
    WriteBytes(projectRoot / "spark.modules.json", "{\"modules\":[{\"path\":\"\xFF\xFEGame.dll\"}]}");
    bool threw = false;
    std::expected<LaunchRequest, std::string> request;
    try
    {
        request = BuildLaunchRequest(binaries, project, LaunchTarget::Game);
    }
    catch (...)
    {
        threw = true;
    }
    EXPECT_FALSE(threw);
    EXPECT_FALSE(request.has_value());

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST(LauncherTemplates_ReadsFieldsAndFallsBackToTheDirectoryName)
{
    using namespace SparkLauncher;
    const auto root = MakeLauncherTestRoot();
    WriteBytes(root / "Templates" / "Arena" / "template.json",
               R"({"name": "Arena Kit", "description": "A small arena.", "genre": "FPS", "gameModule": "ArenaKit"})");
    WriteBytes(root / "Templates" / "Nameless" / "template.json", R"({"description": "no name"})");
    std::filesystem::create_directories(root / "Templates" / "Empty");

    const auto arena = ReadTemplateEntry(root / "Templates" / "Arena");
    ASSERT_TRUE(arena.has_value());
    EXPECT_TRUE(arena->directoryName == "Arena");
    EXPECT_TRUE(arena->displayName == "Arena Kit");
    EXPECT_TRUE(arena->description == "A small arena.");
    EXPECT_TRUE(arena->genre == "FPS");
    EXPECT_TRUE(arena->gameModule == "ArenaKit");

    const auto nameless = ReadTemplateEntry(root / "Templates" / "Nameless");
    ASSERT_TRUE(nameless.has_value());
    EXPECT_TRUE(nameless->displayName == "Nameless");
    EXPECT_TRUE(nameless->description == "no name");

    EXPECT_FALSE(ReadTemplateEntry(root / "Templates" / "Empty").has_value());

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST(LauncherTemplates_RefusedManifestReadsAsEmptyWithoutBlocking)
{
    using namespace SparkLauncher;
    const auto root = MakeLauncherTestRoot();

    // Past the cap the file is not read at all, so its fields are not shown.
    const std::string body = R"({"name": "Huge Kit", "description": "padded"})";
    WriteBytes(root / "Templates" / "Oversized" / "template.json",
               body + std::string(kMaxTemplateManifestBytes + 1 - body.size(), ' '));
    const auto huge = ReadTemplateEntry(root / "Templates" / "Oversized");
    ASSERT_TRUE(huge.has_value());
    EXPECT_TRUE(huge->displayName == "Oversized"); // the directory name, not the file's
    EXPECT_TRUE(huge->description.empty());
    WriteBytes(root / "Templates" / "Oversized" / "template.json",
               body + std::string(kMaxTemplateManifestBytes - body.size(), ' '));
    const auto atCap = ReadTemplateEntry(root / "Templates" / "Oversized");
    ASSERT_TRUE(atCap.has_value());
    EXPECT_TRUE(atCap->displayName == "Huge Kit");

#ifndef _WIN32
    // The launcher read template.json through an ifstream opened by name, which blocked
    // forever when a FIFO sat there.
    std::filesystem::create_directories(root / "Templates" / "Fifo");
    ASSERT_TRUE(::mkfifo((root / "Templates" / "Fifo" / "template.json").c_str(), 0600) == 0);
    const auto fifo = ReadTemplateEntry(root / "Templates" / "Fifo");
    ASSERT_TRUE(fifo.has_value());
    EXPECT_TRUE(fifo->displayName == "Fifo");
    EXPECT_TRUE(fifo->gameModule.empty());
#endif

    std::error_code error;
    std::filesystem::remove_all(root, error);
}
