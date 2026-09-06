/**
 * @file TestEditorProjectMaterializationReal.cpp
 * @brief Real-behavior coverage for how SparkEditor materializes a project from a
 *        template package, and for the metadata contract between the editor's
 *        template registry and the packages themselves.
 *
 * Drives the production SparkEditor::ProjectManager against a staged template
 * root so the assertions are about what a user actually gets:
 *  - the package token is rewritten in every text file, including the `.ini`
 *    service configs;
 *  - build output, a prebuilt `dist/` module, logs and saves never follow a
 *    template into a new project;
 *  - the editor's hard-coded template registry still matches each package's
 *    template.json;
 *  - opening a template package does not rewrite the checked-in package.
 */

#include "TestFramework.h"

#include "Core/ProjectManager.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace SparkEditor;

namespace
{
    namespace fs = std::filesystem;

    fs::path RepositoryRoot()
    {
        return fs::path(SPARK_TEST_SOURCE_DIR);
    }

    fs::path MakeScratchDirectory(const std::string& label)
    {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        const fs::path root =
            fs::temp_directory_path() / ("spark-project-materialization-" + label + "-" + std::to_string(stamp));
        fs::create_directories(root);
        return root;
    }

    void WriteTextFile(const fs::path& path, const std::string& contents)
    {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << contents;
    }

    std::string ReadTextFile(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    /**
     * Stage a template root that looks like a package someone has already built
     * in place: the shipped source files plus build output, a prebuilt module in
     * dist/, a log and a save.
     */
    fs::path StageBuiltFPSStarterTemplateRoot(const fs::path& engineRoot)
    {
        const fs::path source = RepositoryRoot() / "Templates" / "FPSStarter";
        const fs::path package = engineRoot / "Templates" / "FPSStarter";
        fs::create_directories(package);

        for (const char* relative : {"FPSStarter.sparkproject", "template.json", "CMakeLists.txt", "spark.modules.json",
                                     "README.md", "Source/GameModule.h", "Source/GameModule.cpp", "Config/server.ini",
                                     "Config/gateway.ini", "Scenes/Arena.sparkscene"})
        {
            const fs::path destination = package / relative;
            fs::create_directories(destination.parent_path());
            std::error_code ec;
            fs::copy_file(source / relative, destination, fs::copy_options::overwrite_existing, ec);
            if (ec)
                return {};
        }

        WriteTextFile(package / "dist" / "FPSStarter.dll", "stale prebuilt module");
        WriteTextFile(package / "dist" / "Logs" / "engine.log", "stale log");
        WriteTextFile(package / "build" / "CMakeCache.txt", "stale cache");
        WriteTextFile(package / "Saves" / "slot0.spark_save", "stale save");
        WriteTextFile(package / "Logs" / "run.log", "stale log");
        return package;
    }

    /** Minimal string lookup for the small, generated template.json documents. */
    std::string ReadJsonString(const std::string& document, const std::string& key)
    {
        const std::string needle = "\"" + key + "\"";
        const size_t keyPos = document.find(needle);
        if (keyPos == std::string::npos)
            return {};
        const size_t colon = document.find(':', keyPos + needle.size());
        if (colon == std::string::npos)
            return {};
        const size_t open = document.find('"', colon);
        if (open == std::string::npos)
            return {};
        const size_t close = document.find('"', open + 1);
        if (close == std::string::npos)
            return {};
        return document.substr(open + 1, close - open - 1);
    }

    std::vector<std::string> ReadJsonStringArray(const std::string& document, const std::string& key)
    {
        std::vector<std::string> values;
        const std::string needle = "\"" + key + "\"";
        const size_t keyPos = document.find(needle);
        if (keyPos == std::string::npos)
            return values;
        const size_t open = document.find('[', keyPos);
        const size_t close = document.find(']', open == std::string::npos ? keyPos : open);
        if (open == std::string::npos || close == std::string::npos)
            return values;

        size_t cursor = open + 1;
        while (cursor < close)
        {
            const size_t quote = document.find('"', cursor);
            if (quote == std::string::npos || quote > close)
                break;
            const size_t end = document.find('"', quote + 1);
            if (end == std::string::npos || end > close)
                break;
            values.push_back(document.substr(quote + 1, end - quote - 1));
            cursor = end + 1;
        }
        return values;
    }
} // namespace

TEST(ProjectMaterialization_RewritesThePackageTokenInServiceConfigs)
{
    const fs::path scratch = MakeScratchDirectory("token");
    const fs::path engineRoot = scratch / "Engine";
    ASSERT_FALSE(StageBuiltFPSStarterTemplateRoot(engineRoot).empty());

    fs::create_directories(scratch / "Projects");
    ProjectManager manager;
    manager.Initialize();
    manager.SetEngineRoot(engineRoot.string());
    EXPECT_TRUE(manager.CreateProject("TokenGame", (scratch / "Projects").string(), ProjectTemplate::FirstPerson,
                                      "materialization test"));

    const fs::path projectRoot = scratch / "Projects" / "TokenGame";
    const std::string serverConfig = ReadTextFile(projectRoot / "Config" / "server.ini");
    const std::string gatewayConfig = ReadTextFile(projectRoot / "Config" / "gateway.ini");
    EXPECT_TRUE(!serverConfig.empty());
    EXPECT_STR_CONTAINS(serverConfig, "name = TokenGame Dedicated Server");
    EXPECT_STR_CONTAINS(gatewayConfig, "world_name = TokenGame");

    // Nothing in the materialized project may still carry the package token.
    std::vector<std::string> offenders;
    for (const auto& entry : fs::recursive_directory_iterator(projectRoot))
    {
        if (!entry.is_regular_file())
            continue;
        const std::string extension = entry.path().extension().string();
        if (extension != ".ini" && extension != ".json" && extension != ".cmake" && extension != ".txt" &&
            extension != ".h" && extension != ".cpp" && extension != ".md" && extension != ".sparkproject")
            continue;
        if (ReadTextFile(entry.path()).find("FPSStarter") != std::string::npos)
            offenders.push_back(entry.path().filename().string());
    }
    EXPECT_EQ(offenders.size(), static_cast<size_t>(0));

    std::error_code cleanupEc;
    fs::remove_all(scratch, cleanupEc);
}

TEST(ProjectMaterialization_LeavesBuildOutputDistLogsAndSavesBehind)
{
    const fs::path scratch = MakeScratchDirectory("exclusions");
    const fs::path engineRoot = scratch / "Engine";
    const fs::path package = StageBuiltFPSStarterTemplateRoot(engineRoot);
    ASSERT_FALSE(package.empty());
    ASSERT_TRUE(fs::is_regular_file(package / "dist" / "FPSStarter.dll"));

    fs::create_directories(scratch / "Projects");
    ProjectManager manager;
    manager.Initialize();
    manager.SetEngineRoot(engineRoot.string());
    EXPECT_TRUE(manager.CreateProject("CleanGame", (scratch / "Projects").string(), ProjectTemplate::FirstPerson,
                                      "materialization test"));

    const fs::path projectRoot = scratch / "Projects" / "CleanGame";
    EXPECT_FALSE(fs::exists(projectRoot / "dist"));
    EXPECT_FALSE(fs::exists(projectRoot / "build"));
    EXPECT_FALSE(fs::exists(projectRoot / "Saves"));
    EXPECT_FALSE(fs::exists(projectRoot / "Logs"));

    // The template's real sources still arrive.
    EXPECT_TRUE(fs::is_regular_file(projectRoot / "Source" / "GameModule.h"));
    EXPECT_TRUE(fs::is_regular_file(projectRoot / "Scenes" / "Arena.sparkscene"));
    EXPECT_TRUE(fs::is_regular_file(projectRoot / "Config" / "server.ini"));

    std::error_code cleanupEc;
    fs::remove_all(scratch, cleanupEc);
}

TEST(ProjectMaterialization_EditorRegistryMatchesEveryPackageTemplateJson)
{
    size_t checked = 0;
    for (const auto& descriptor : ProjectManager::GetProjectTemplateDescriptors())
    {
        const fs::path metadataPath =
            RepositoryRoot() / "Templates" / std::string(descriptor.packageDirectory) / "template.json";
        ASSERT_TRUE(fs::is_regular_file(metadataPath));
        const std::string metadata = ReadTextFile(metadataPath);

        EXPECT_EQ(ReadJsonString(metadata, "identity"), std::string(descriptor.stableId));
        EXPECT_EQ(ReadJsonString(metadata, "description"), std::string(descriptor.description));
        EXPECT_EQ(ReadJsonString(metadata, "genre"), std::string(descriptor.genre));
        EXPECT_EQ(ReadJsonString(metadata, "defaultScene"), std::string(descriptor.defaultScene));
        EXPECT_EQ(ReadJsonString(metadata, "gameModule"), std::string(descriptor.packageDirectory));

        const std::vector<std::string> packageFeatures = ReadJsonStringArray(metadata, "features");
        EXPECT_EQ(packageFeatures.size(), descriptor.features.size());
        if (packageFeatures.size() == descriptor.features.size())
        {
            for (size_t i = 0; i < packageFeatures.size(); ++i)
                EXPECT_EQ(packageFeatures[i], std::string(descriptor.features[i]));
        }
        ++checked;
    }
    EXPECT_EQ(checked, static_cast<size_t>(8));
}

TEST(ProjectMaterialization_EveryShippedPackageDocumentIsUniform)
{
    size_t checked = 0;
    for (const auto& entry : fs::directory_iterator(RepositoryRoot() / "Templates"))
    {
        if (!entry.is_directory())
            continue;
        const std::string package = entry.path().filename().string();
        const fs::path document = entry.path() / (package + ".sparkproject");
        if (!fs::is_regular_file(document))
            continue;

        const std::string contents = ReadTextFile(document);
        EXPECT_STR_CONTAINS(contents, "\"projectFileVersion\": 1");
        EXPECT_EQ(ReadJsonString(contents, "name"), package);
        // Every package - the legacy MultiplayerArena included - carries a
        // template identity, so the launcher and editor read one document shape.
        EXPECT_TRUE(!ReadJsonString(contents, "template").empty());
        EXPECT_STR_CONTAINS(contents, "\"createdTime\": 0");
        EXPECT_STR_CONTAINS(contents, "\"lastModified\": 0");
        // The package description is the one template.json publishes.
        EXPECT_EQ(ReadJsonString(contents, "description"),
                  ReadJsonString(ReadTextFile(entry.path() / "template.json"), "description"));
        ++checked;
    }
    EXPECT_EQ(checked, static_cast<size_t>(9));
}

TEST(ProjectMaterialization_OpeningATemplatePackageNeverRewritesIt)
{
    const fs::path scratch = MakeScratchDirectory("readonly");
    const fs::path engineRoot = scratch / "Engine";
    const fs::path package = StageBuiltFPSStarterTemplateRoot(engineRoot);
    ASSERT_FALSE(package.empty());

    const fs::path document = package / "FPSStarter.sparkproject";
    const std::string before = ReadTextFile(document);
    EXPECT_STR_CONTAINS(before, "\"lastModified\": 0");

    ProjectManager manager;
    manager.Initialize();
    manager.SetEngineRoot(engineRoot.string());
    EXPECT_TRUE(manager.OpenProject(document.string()));
    EXPECT_EQ(ReadTextFile(document), before);

    // An explicit save must refuse rather than dirty the checked-in package.
    EXPECT_FALSE(manager.SaveProject());
    EXPECT_EQ(ReadTextFile(document), before);

    std::error_code cleanupEc;
    fs::remove_all(scratch, cleanupEc);
}
