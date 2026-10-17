/**
 * @file FuzzLauncherModuleManifestProduction.cpp
 * @brief libc++-compiled production adapter for the launcher spark.modules.json libFuzzer harness.
 *
 * To launch a project's game, SparkLauncher::BuildLaunchRequest (SparkLauncher/src/
 * LauncherProcess.cpp) reads the project's spark.modules.json, resolves every module "path"
 * to a native module with a .sparkabi sidecar, and writes the resolved manifest to
 * build/.spark-launcher/spark.modules.json for the engine. The project is whatever the user
 * opened, so the manifest is untrusted. The adapter builds one fixture project:
 *
 *   <base>/Release/SparkEngine                              the binary directory
 *   <base>/proj/Game.sparkproject, <base>/proj/Game         (a plain file named like a module)
 *   <base>/proj/build/Release/libGame.so (+ .sparkabi)      a built module
 *   <base>/proj/build/Release/libTool.so                    a module without a sidecar
 *   <base>/proj/build/libDup.so, build/Release/libDup.so    (+ sidecars) an ambiguous module
 *   <base>/proj/mods/Local.so (+ .sparkabi)                 a module named by relative path
 *   <base>/proj/mods/LinkedEvil.so -> ../../outside/Evil.so (+ .sparkabi beside the link)
 *   <base>/proj/escape -> ../outside                        a directory link out of the project
 *   <base>/outside/Evil.so (+ .sparkabi)                    a module outside the project
 *
 * and plants the fuzz bytes as <base>/proj/spark.modules.json. A violated contract aborts so
 * libFuzzer records a crash:
 *  - a refused manifest writes no resolved manifest,
 *  - an accepted one yields a request for <base>/Release/SparkEngine with exactly the
 *    "-manifest <resolved> --project <project>" arguments, and a resolved manifest that is
 *    strict JSON holding as many modules as the input, each with a "path" naming one of the
 *    fixture modules that has a sidecar (never libTool.so, never a link, never a path with NUL),
 *  - the same manifest gives the same answer and the same resolved file twice.
 */

#include "FuzzLauncherModuleManifestProduction.h"

#include "LauncherProcess.h"
#include "Utils/JsonUtils.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 1024u * 1024u;
    namespace fs = std::filesystem;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzLauncherModuleManifest: BuildLaunchRequest violated: %s\n", what);
        std::abort();
    }

    void WriteFile(const fs::path& path, std::string_view bytes)
    {
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out)
        {
            InvariantFailure("could not write a fixture file");
        }
    }

    std::string ReadFile(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    struct Fixture
    {
        fs::path binaries;
        fs::path project;
        fs::path manifest;
        fs::path resolved;
        std::vector<std::string> allowedModules; // PathToUtf8 spellings
    };

    void PlantModule(const fs::path& module, bool withSidecar)
    {
        WriteFile(module, "\x7f"
                          "ELF fixture module");
        if (withSidecar)
        {
            fs::path sidecar = module;
            sidecar += ".sparkabi";
            WriteFile(sidecar, "fixture sidecar");
        }
    }

    Fixture MakeFixture()
    {
        std::string pattern = (fs::temp_directory_path() / "spark-fuzz-launcher-XXXXXX").string();
        if (::mkdtemp(pattern.data()) == nullptr)
        {
            InvariantFailure("could not create the fixture directory");
        }
        const fs::path base = fs::canonical(pattern);
        const fs::path project = base / "proj";

        WriteFile(base / "Release" / "SparkEngine", "fixture engine");
        WriteFile(project / "Game.sparkproject", "{}");
        WriteFile(project / "Game", "a plain file whose name a module path could be cut down to");
        PlantModule(project / "build" / "Release" / "libGame.so", true);
        PlantModule(project / "build" / "Release" / "libTool.so", false);
        PlantModule(project / "build" / "Release" / "libDup.so", true);
        PlantModule(project / "build" / "libDup.so", true);
        PlantModule(project / "mods" / "Local.so", true);
        PlantModule(base / "outside" / "Evil.so", true);
        fs::create_symlink("../../outside/Evil.so", project / "mods" / "LinkedEvil.so");
        WriteFile(project / "mods" / "LinkedEvil.so.sparkabi", "sidecar beside a link");
        fs::create_directory_symlink("../outside", project / "escape");

        Fixture fixture;
        fixture.binaries = base / "Release";
        fixture.project = project / "Game.sparkproject";
        fixture.manifest = project / "spark.modules.json";
        fixture.resolved = project / "build" / ".spark-launcher" / "spark.modules.json";
        for (const fs::path& module :
             {project / "build" / "Release" / "libGame.so", project / "build" / "Release" / "libDup.so",
              project / "build" / "libDup.so", project / "mods" / "Local.so", base / "outside" / "Evil.so"})
            fixture.allowedModules.push_back(SparkLauncher::PathToUtf8(module));
        return fixture;
    }

    std::size_t InputModuleCount(std::string_view json)
    {
        Spark::Json::JsonLimits limits;
        limits.maxBytes = SparkLauncher::kMaxModuleManifestBytes;
        Spark::Json::Value root;
        if (!Spark::Json::ParseBounded(json, limits, &root) || !root.IsObject() || !root.HasKey("modules") ||
            !root["modules"].IsArray())
            InvariantFailure("an accepted manifest is not an object with a modules array");
        return root["modules"].Size();
    }

    void CheckResolvedManifest(const Fixture& fixture, std::string_view input, const std::string& resolvedText)
    {
        Spark::Json::Value root;
        if (!Spark::Json::ParseStrict(resolvedText, &root) || !root.IsObject() || !root.HasKey("modules"))
        {
            InvariantFailure("the resolved manifest is not strict JSON with a modules key");
        }
        const Spark::Json::Value& modules = root["modules"];
        if (!modules.IsArray() || modules.Size() == 0 || modules.Size() != InputModuleCount(input))
        {
            InvariantFailure("the resolved manifest does not hold one module per input module");
        }
        for (std::size_t i = 0; i < modules.Size(); ++i)
        {
            const Spark::Json::Value& entry = modules[i];
            if (!entry.IsObject() || !entry.HasKey("path") || !entry["path"].IsString())
            {
                InvariantFailure("a resolved module has no string path");
            }
            const std::string& path = entry["path"].AsString();
            if (path.find('\0') != std::string::npos)
            {
                InvariantFailure("a resolved module path holds a NUL");
            }
            if (std::find(fixture.allowedModules.begin(), fixture.allowedModules.end(), path) ==
                fixture.allowedModules.end())
                InvariantFailure("a module path resolved to something other than a sidecar-backed fixture module");
        }
    }
} // namespace

extern "C" int SparkFuzzResolveLauncherManifest(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    static const Fixture fixture = MakeFixture();
    const std::string_view input =
        size == 0 ? std::string_view() : std::string_view(reinterpret_cast<const char*>(data), size);
    WriteFile(fixture.manifest, input);

    std::error_code error;
    fs::remove(fixture.resolved, error);
    const auto request =
        SparkLauncher::BuildLaunchRequest(fixture.binaries, fixture.project, SparkLauncher::LaunchTarget::Game);
    if (!request)
    {
        if (fs::exists(fixture.resolved, error))
        {
            InvariantFailure("a refused manifest wrote a resolved manifest");
        }
        const auto again =
            SparkLauncher::BuildLaunchRequest(fixture.binaries, fixture.project, SparkLauncher::LaunchTarget::Game);
        if (again || again.error() != request.error())
        {
            InvariantFailure("the same manifest gave different answers");
        }
        return 0;
    }

    if (request->executable != fixture.binaries / "SparkEngine")
    {
        InvariantFailure("the request does not run the binary directory's SparkEngine");
    }
    const std::array<std::string, 4> expected = {"-manifest", SparkLauncher::PathToUtf8(fixture.resolved), "--project",
                                                 SparkLauncher::PathToUtf8(fixture.project)};
    if (!std::equal(request->arguments.begin(), request->arguments.end(), expected.begin(), expected.end()))
    {
        InvariantFailure("the request arguments are not -manifest <resolved> --project <project>");
    }
    const std::string resolvedText = ReadFile(fixture.resolved);
    CheckResolvedManifest(fixture, input, resolvedText);

    fs::remove(fixture.resolved, error);
    const auto again =
        SparkLauncher::BuildLaunchRequest(fixture.binaries, fixture.project, SparkLauncher::LaunchTarget::Game);
    if (!again || again->arguments != request->arguments || ReadFile(fixture.resolved) != resolvedText)
    {
        InvariantFailure("the same manifest resolved differently twice");
    }
    return 0;
}
