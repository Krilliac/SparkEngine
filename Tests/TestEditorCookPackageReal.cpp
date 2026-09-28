/**
 * @file TestEditorCookPackageReal.cpp
 * @brief EDT-210: an editor-authored asset assignment is saved, cooked, packaged and run.
 *
 * One production-linked scenario from authoring to a running packaged host:
 *   1. Author: a project document opened and rewritten by ProjectManager, a World entity
 *      with Transform and MeshRenderer, and the mesh assigned through the Asset Browser
 *      drag payload (MakeAssetDragReference, DecodeAssetDragPayload) and the Inspector's
 *      drop core (ApplyWorldAssetDrop) as exactly one CommandHistory entry recorded by the
 *      editor's EditorDocument, then saved with
 *      Spark::SaveWorld and recorded as the project's opened scene.
 *   2. Cook: BuildPipeline::StartCookOnly runs the real SparkCooker found beside the test
 *      host; the cook manifest must list the dragged mesh.
 *   3. Package: BuildPipeline::AssembleNativePackage assembles the cook output (a
 *      project-shaped root with the cooked Assets/) around the real SparkEngine host named by
 *      SPARK_ENGINE_EXECUTABLE. The editor's Build + Package action assembles from the
 *      project root instead; packaging the cook output here makes the run consume the cooked
 *      content. The game module is a placeholder file plus .sparkabi sidecar: the
 *      scene-preview launch mode never loads modules, so module execution is out of scope.
 *   4. Run: the packaged ScenePreview host runs headless with `-scene
 *      Scenes/Startup.sparkscene` from the package root, as LaunchScene does, and must
 *      report the authored entity, its renderable and its resolved mesh reference.
 * The negative case removes the cooked mesh from the package and requires the same run to
 * report it missing, so the asset record is not a constant.
 *
 * Two lanes run this family. EditorCookPackageRoundTrip packages the build-tree
 * SparkEngine. EditorCookPackageInstalledRuntime (cmake/RunEditorAuthorInstalledRuntime.cmake)
 * installs the runtime component into a fresh prefix outside the source and build trees
 * and packages that installed host; it also sets SPARK_ENGINE_INSTALLED_PREFIX, and the
 * tests then require the host to lie under that prefix with no CMakeCache.txt above it,
 * so a build-tree host cannot satisfy the installed-runtime lane.
 *
 * SPARK_ENGINE_EXECUTABLE is set by both ctest registrations; without it the tests fail
 * rather than skip. Both lanes are Windows-only, so the tests are compiled only under
 * _WIN32: other hosts' whole-binary runs (sanitizers, coverage) never see a family they
 * cannot satisfy.
 */

#include "TestFramework.h"

#include "AssetPipeline/EditorAssetDrag.h"
#include "CommandHistory.h"
#include "Core/EditorDocument.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/CoreComponents.h"
#include "Fixtures/ScopedEditorProfile.h"
#include "Panels/BuildPipeline.h"
#include "Panels/InspectorWorldAssetDrop.h"
#include "SceneManager/ReflectedSceneSerializer.h"
#include "Utils/Process.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)

namespace
{
    namespace fs = std::filesystem;
    using SparkEditor::BuildPipeline;
    using SparkEditor::BuildResult;

    constexpr const char* kProjectName = "CrateProject";
    constexpr const char* kExecutableName = "Crate Game";
    constexpr const char* kMeshReference = "Assets/Meshes/crate.obj";
    constexpr int kTestFrames = 3;

#ifdef _WIN32
    constexpr const char* kModuleFilename = "CrateGame.dll";
    constexpr const char* kScenePreviewHost = "Crate Game Scene.exe";
#elif defined(__APPLE__)
    constexpr const char* kModuleFilename = "CrateGame.dylib";
    constexpr const char* kScenePreviewHost = "Crate Game Scene";
#else
    constexpr const char* kModuleFilename = "CrateGame.so";
    constexpr const char* kScenePreviewHost = "Crate Game Scene";
#endif

    /// A unit cube: eight vertices, twelve triangles.
    constexpr const char* kCrateObj = "v -0.5 -0.5 -0.5\nv 0.5 -0.5 -0.5\nv 0.5 0.5 -0.5\nv -0.5 0.5 -0.5\n"
                                      "v -0.5 -0.5 0.5\nv 0.5 -0.5 0.5\nv 0.5 0.5 0.5\nv -0.5 0.5 0.5\n"
                                      "f 1 3 2\nf 1 4 3\nf 5 6 7\nf 5 7 8\nf 1 2 6\nf 1 6 5\n"
                                      "f 4 7 3\nf 4 8 7\nf 1 5 8\nf 1 8 4\nf 2 3 7\nf 2 7 6\n";

    Spark::Editor::CommandHistory& History()
    {
        return Spark::Editor::CommandHistory::GetInstance();
    }

    /// The document APIs take UTF-8; path::string() is the ANSI code page on Windows.
    std::string Utf8(const fs::path& path)
    {
        const std::u8string utf8 = path.u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }

    std::string ReadText(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    size_t CountOccurrences(const std::string& text, const std::string& needle)
    {
        size_t count = 0;
        for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
            ++count;
        return count;
    }

    struct HostRun
    {
        int exitCode = -1;
        std::string output;
    };

    /// A scratch project authored, cooked and packaged through the production editor paths.
    class CookPackageScenario
    {
      public:
        explicit CookPackageScenario(const char* tag)
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            m_root = fs::temp_directory_path() /
                     ("spark-edt210-cook-package-" + std::string(tag) + "-" + std::to_string(stamp));
            m_project = m_root / kProjectName;
            m_package = m_root / "Package";
        }

        ~CookPackageScenario()
        {
            History().Clear();
            std::error_code ec;
            fs::remove_all(m_root, ec);
        }

        CookPackageScenario(const CookPackageScenario&) = delete;
        CookPackageScenario& operator=(const CookPackageScenario&) = delete;

        const fs::path& Package() const { return m_package; }

        /// Steps 1-3; any failure aborts the calling test.
        void AuthorCookAndPackage()
        {
            // Resolved first: a missing host must fail the test before any work is done.
            const char* engine = std::getenv("SPARK_ENGINE_EXECUTABLE");
            if (!engine || !*engine)
                std::cerr << "  SPARK_ENGINE_EXECUTABLE is not set; run through the EditorCookPackageRoundTrip ctest\n";
            ASSERT_TRUE(engine && *engine);
            const fs::path host(engine);
            ASSERT_TRUE(fs::is_regular_file(host));
            ExpectInstalledHost(host);

            Author();
            Cook();
            Assemble(host);
        }

        /// Step 4: the packaged scene-preview host, launched the way LaunchScene does.
        HostRun RunPackagedScene() const
        {
            HostRun run;
            auto launched = Spark::Process::Builder(Utf8(m_package / "ScenePreview" / kScenePreviewHost))
                                .Arg("-headless")
                                .Arg("-test-frames")
                                .Arg(std::to_string(kTestFrames))
                                .Arg("-no-subprocess")
                                .Arg("-scene")
                                .Arg("Scenes/Startup.sparkscene")
                                .WorkingDirectory(Utf8(m_package))
                                .CaptureStdout()
                                .MergeStderrIntoStdout()
                                .NoWindow()
                                .Launch();
            if (!launched)
            {
                run.output = "launch failed: " + launched.error();
                return run;
            }
            Spark::Process& process = *launched;
            std::string line;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
            while (process.IsRunning() && std::chrono::steady_clock::now() < deadline)
            {
                while (process.TryReadLine(line))
                    run.output += line + '\n';
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (process.IsRunning())
            {
                process.Kill();
                run.output += "<killed after 120 s>\n";
                return run;
            }
            run.output += process.ReadAllStdout();
            run.exitCode = process.WaitForExit();
            run.output.erase(std::remove(run.output.begin(), run.output.end(), '\r'), run.output.end());
            return run;
        }

      private:
        /// Under the installed-runtime lane the host must come from the install prefix, and
        /// no build tree (a CMakeCache.txt) may sit anywhere above it.
        static void ExpectInstalledHost(const fs::path& host)
        {
            const char* installed = std::getenv("SPARK_ENGINE_INSTALLED_PREFIX");
            if (!installed || !*installed)
                return;
            std::error_code ec;
            const fs::path prefix = fs::weakly_canonical(fs::path(installed), ec);
            ASSERT_FALSE(ec);
            const fs::path hostPath = fs::weakly_canonical(host, ec);
            ASSERT_FALSE(ec);
            const fs::path relative = hostPath.lexically_relative(prefix);
            const bool underPrefix = !relative.empty() && *relative.begin() != fs::path("..");
            if (!underPrefix)
                std::cerr << "  host " << Utf8(hostPath) << " is not under the installed prefix " << Utf8(prefix)
                          << "\n";
            ASSERT_TRUE(underPrefix);
            for (fs::path directory = hostPath.parent_path(); !directory.empty(); directory = directory.parent_path())
            {
                const bool buildTree = fs::exists(directory / "CMakeCache.txt", ec);
                if (buildTree)
                    std::cerr << "  installed host sits below the build tree " << Utf8(directory) << "\n";
                ASSERT_FALSE(buildTree);
                if (directory == directory.parent_path())
                    break;
            }
        }

        void Author()
        {
            const fs::path assets = m_project / "Assets";
            const fs::path mesh = assets / "Meshes" / "crate.obj";
            fs::create_directories(mesh.parent_path());
            fs::create_directories(m_project / "Scenes");
            fs::create_directories(m_project / "Config");
            std::ofstream(mesh, std::ios::binary) << kCrateObj;
            std::ofstream(m_project / "Config" / "Game.json", std::ios::binary) << "{}\n";
            const fs::path document = m_project / (std::string(kProjectName) + ".sparkproject");
            std::ofstream(document, std::ios::binary)
                << "{\n  \"projectFileVersion\": 1,\n  \"name\": \"" << kProjectName
                << "\",\n  \"defaultScene\": \"Scenes/Default.sparkscene\",\n  \"modules\": [],\n  \"scenes\": []\n}\n";

            SparkEditor::Testing::IsolatedProjectManager projects;
            projects.Initialize();
            ASSERT_TRUE(projects.OpenProject(Utf8(document)));

            SparkEditor::EditorDocument document;
            auto authored = std::make_unique<::World>();
            const ::EntityID crate = authored->CreateEntity("Crate");
            authored->AddComponent<::Transform>(crate);
            authored->AddComponent<::MeshRenderer>(crate);
            document.ReplaceWorld(std::move(authored));
            ::World& world = *document.GetWorld();

            // The Asset Browser's drag payload, received by the Inspector's mesh slot.
            const std::string reference = SparkEditor::MakeAssetDragReference(mesh, assets);
            ASSERT_EQ(reference, std::string(kMeshReference));
            const std::string payload = reference + '\0';
            std::string received;
            ASSERT_TRUE(SparkEditor::DecodeAssetDragPayload(payload.data(), static_cast<int>(payload.size()),
                                                            SparkEditor::EditorAssetKind::Mesh, received));

            // The drop commits through the document's applied-edit command, as EditorUI records it.
            History().Clear();
            const auto snapshot = [&document]() { return document.Capture(); };
            const SparkEditor::InspectorPendingWorldEdit::CommitFn commit =
                [&document](const std::string& before, const std::string& description)
            { return document.RecordApplied(before, description); };
            ASSERT_EQ(static_cast<int>(SparkEditor::ApplyWorldAssetDrop(world, crate, "MeshRenderer", "meshPath",
                                                                        received, snapshot, commit)),
                      static_cast<int>(SparkEditor::AssetDropResult::Applied));
            ASSERT_EQ(History().UndoCount(), static_cast<size_t>(1));
            EXPECT_STR_CONTAINS(History().GetUndoDescription(), "Assign Mesh Asset");

            std::string error;
            ASSERT_TRUE(Spark::SaveWorld(world, Utf8(m_project / "Scenes" / "Default.sparkscene"), &error));
            ASSERT_TRUE(projects.RecordOpenedScene("Scenes/Default.sparkscene"));
            EXPECT_STR_CONTAINS(ReadText(document), "\"lastOpenedScene\": \"Scenes/Default.sparkscene\"");
            projects.RemoveRecentProject(Utf8(document));
            projects.CloseProject();
            // The recorded commands capture this function's document.
            History().Clear();
        }

        void Cook()
        {
            SparkEditor::BuildSettings settings = PackageSettings();
            BuildPipeline pipeline;
            ASSERT_TRUE(pipeline.StartCookOnly(settings, m_project.string()));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
            while (pipeline.IsRunning() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            ASSERT_FALSE(pipeline.IsRunning());
            if (pipeline.GetResult() != BuildResult::Success)
            {
                for (const SparkEditor::BuildLogLine& line : pipeline.DrainLogLines())
                    std::cerr << "  cook: " << line.text << "\n";
            }
            ASSERT_TRUE(pipeline.GetResult() == BuildResult::Success);

            const fs::path cooked = m_project / settings.outputDirectory;
            EXPECT_STR_CONTAINS(ReadText(cooked / "Assets" / "spark-cook-manifest.json"),
                                "\"path\": \"Meshes/crate.obj\"");
            ASSERT_TRUE(fs::is_regular_file(cooked / "Assets" / "Meshes" / "crate.obj"));
            ASSERT_TRUE(fs::is_regular_file(cooked / "Scenes" / "Default.sparkscene"));
            ASSERT_TRUE(fs::is_regular_file(cooked / (std::string(kProjectName) + ".sparkproject")));
        }

        void Assemble(const fs::path& host)
        {
            const fs::path artifacts = m_root / "Artifacts";
            fs::create_directories(artifacts);
            std::ofstream(artifacts / kModuleFilename, std::ios::binary) << "placeholder module";
            std::ofstream(artifacts / (std::string(kModuleFilename) + ".sparkabi"), std::ios::binary) << "abi";

            const SparkEditor::BuildSettings settings = PackageSettings();
            std::string error;
            const bool assembled = BuildPipeline::AssembleNativePackage(
                settings, (m_project / settings.outputDirectory).string(), host.string(),
                (artifacts / kModuleFilename).string(), m_package.string(), &error);
            if (!assembled)
                std::cerr << "  package: " << error << "\n";
            ASSERT_TRUE(assembled);
            ASSERT_TRUE(fs::is_regular_file(m_package / "ScenePreview" / kScenePreviewHost));
            ASSERT_TRUE(fs::is_regular_file(m_package / "Scenes" / "Startup.sparkscene"));
            ASSERT_TRUE(fs::is_regular_file(m_package / "Assets" / "Meshes" / "crate.obj"));
            EXPECT_STR_CONTAINS(ReadText(m_package / "Scenes" / "Startup.sparkscene"), kMeshReference);
        }

        static SparkEditor::BuildSettings PackageSettings()
        {
            SparkEditor::BuildSettings settings;
            settings.platform = BuildPipeline::NativeTargetPlatform();
            settings.executableName = kExecutableName;
            settings.packageDedicatedServer = false;
            settings.cookAssets = true;
            settings.outputDirectory = "Cooked";
            return settings;
        }

        fs::path m_root;
        fs::path m_project;
        fs::path m_package;
    };

    void ExpectSceneRecords(const HostRun& run, size_t missingAssets)
    {
        const std::string loaded = "SPARK_SCENE_LOADED entities=1 renderables=1\n";
        const std::string assets = "SPARK_SCENE_ASSETS refs=1 missing=" + std::to_string(missingAssets) + "\n";
        // The headless loop ran over the loaded scene, and no game module took it over.
        const std::string loop =
            "SPARK_HEADLESS_RHI backend=null initialized=1 frames=" + std::to_string(kTestFrames) + " shutdown=1\n";
        const bool singleScene = CountOccurrences(run.output, "SPARK_SCENE_LOADED ") == 1;
        const bool noModule = run.output.find("SPARK_MODULE_READY") == std::string::npos;
        const bool recordsMatch = run.output.find(loaded) != std::string::npos &&
                                  run.output.find(assets) != std::string::npos &&
                                  run.output.find(loop) != std::string::npos;
        // The framework truncates a failed haystack, so print the whole run once.
        if (run.exitCode != 0 || !singleScene || !noModule || !recordsMatch)
            std::cerr << "  packaged scene run exited " << run.exitCode << ":\n" << run.output << "\n";
        EXPECT_EQ(run.exitCode, 0);
        EXPECT_TRUE(singleScene);
        EXPECT_TRUE(noModule);
        EXPECT_STR_CONTAINS(run.output, loaded);
        EXPECT_STR_CONTAINS(run.output, assets);
        EXPECT_STR_CONTAINS(run.output, loop);
    }
} // namespace

TEST(EditorCookPackage_AuthoredDropSavesCooksPackagesAndRunsScene)
{
    CookPackageScenario scenario("run");
    scenario.AuthorCookAndPackage();
    ExpectSceneRecords(scenario.RunPackagedScene(), 0);
}

TEST(EditorCookPackage_MissingCookedAssetIsReportedByPackagedRun)
{
    CookPackageScenario scenario("missing");
    scenario.AuthorCookAndPackage();
    ASSERT_TRUE(fs::remove(scenario.Package() / "Assets" / "Meshes" / "crate.obj"));
    ExpectSceneRecords(scenario.RunPackagedScene(), 1);
}

#endif // defined(_WIN32)
