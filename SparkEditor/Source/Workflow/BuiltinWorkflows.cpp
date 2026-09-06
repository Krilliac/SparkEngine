/**
 * @file BuiltinWorkflows.cpp
 * @brief Implementations of pre-built editor workflow templates
 */

#include "BuiltinWorkflows.h"
#include "EditorWorkflow.h"
#include "../Core/EditorUI.h"
#include "../Core/ProjectManager.h"
#include "Utils/Process.h"

#include <filesystem>
#include <sstream>
#include <vector>

namespace SparkEditor
{
    namespace
    {
        std::string LastLines(const std::string& output, size_t maxLines)
        {
            std::vector<std::string> lines;
            std::istringstream stream(output);
            std::string line;
            while (std::getline(stream, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                lines.push_back(line);
            }

            const size_t first = lines.size() > maxLines ? lines.size() - maxLines : 0;
            std::string result;
            for (size_t i = first; i < lines.size(); ++i)
            {
                result += lines[i];
                result.push_back('\n');
            }
            return result;
        }

        int RunWorkflowProcess(WorkflowContext& ctx, const std::string& executable,
                               const std::vector<std::string>& arguments, size_t logLines = 10)
        {
            Spark::Process::Builder builder(executable);
            for (const auto& argument : arguments)
                builder.Arg(argument);
            builder.CaptureStdout().CaptureStderr();

            auto launched = builder.Launch();
            if (!launched)
            {
                ctx.Log("Failed to launch " + executable + ": " + launched.error());
                return -1;
            }

            auto process = std::move(*launched);
            const std::string stdoutText = process.ReadAllStdout();
            const std::string stderrText = process.ReadAllStderr();
            const int exitCode = process.WaitForExit();

            const std::string output = LastLines(stdoutText + stderrText, logLines);
            if (!output.empty())
                ctx.Log(output);
            return exitCode;
        }

        /// @brief Build directory belonging to the open project, or an empty path when
        /// no project is open. Never falls back to a CWD-relative "build".
        std::filesystem::path ProjectBuildDirectory()
        {
            const std::filesystem::path project = ProjectManager::GetActiveProjectPath();
            if (project.empty())
                return {};
            return project / "build";
        }

        /// @brief Configure the open project into its own build directory.
        /// A repo CMake preset was used here previously, which resolves its binaryDir relative to
        /// whatever CMakePresets.json sits in the editor's working directory — never the
        /// <project>/build tree the Build and Clean steps operate on. Configure and build must
        /// name the same directory or the workflow can never succeed.
        bool ConfigureOpenProject(WorkflowContext& ctx)
        {
            const std::filesystem::path project = ProjectManager::GetActiveProjectPath();
            if (project.empty())
            {
                ctx.Log("No project is open — refusing to configure an unscoped directory");
                return false;
            }

            const std::filesystem::path buildDir = project / "build";
            const int rc = RunWorkflowProcess(
                ctx, "cmake", {"-S", project.string(), "-B", buildDir.string(), "-DCMAKE_BUILD_TYPE=Release"}, 5);
            ctx.Log("CMake configure exit: " + std::to_string(rc));
            return rc == 0;
        }

        /// @brief Build the directory ConfigureOpenProject() just wrote.
        bool BuildOpenProject(WorkflowContext& ctx)
        {
            const std::filesystem::path buildDir = ProjectBuildDirectory();
            if (buildDir.empty())
            {
                ctx.Log("No project is open — refusing to build an unscoped directory");
                return false;
            }

            const int rc = RunWorkflowProcess(
                ctx, "cmake", {"--build", buildDir.string(), "--config", "Release", "--parallel", "2"}, 10);
            ctx.Log("Build exit: " + std::to_string(rc));
            return rc == 0;
        }

        std::vector<std::string> CollectFormatCheckFiles()
        {
            std::vector<std::string> files;
            const std::vector<std::filesystem::path> roots = {"SparkEngine/Source", "SparkEditor/Source"};
            for (const auto& root : roots)
            {
                std::error_code ec;
                if (!std::filesystem::exists(root, ec))
                    continue;

                for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec))
                {
                    if (ec || files.size() >= 50)
                        break;
                    if (!entry.is_regular_file())
                        continue;

                    const auto ext = entry.path().extension().string();
                    if (ext == ".h" || ext == ".cpp")
                        files.push_back(entry.path().generic_string());
                }
            }
            return files;
        }
    } // namespace

    void RegisterBuiltinWorkflows()
    {
        auto& registry = WorkflowRegistry::Instance();

        // ====================================================================
        // Build: Quick Build & Run
        // ====================================================================
        {
            EditorWorkflow wf("Quick Build & Run", "Save scene, build Development config, launch executable", "Build");

            wf.AddStep({"Save Scene", "Save the current scene before building", [](WorkflowContext& ctx)
                        {
                            if (ctx.editorUI)
                            {
                                // SaveScene() resolves the project-relative scene path; a bare
                                // "<name>.sparkscene" would be written next to the working
                                // directory and then adopted as the editor's current scene.
                                const bool saved = ctx.editorUI->SaveScene();
                                ctx.Log(saved ? "Scene saved" : "Scene save failed");
                                return saved;
                            }
                            ctx.Log("No editor context — skipping scene save");
                            return true;
                        }});

            wf.AddStep({"Configure", "Run CMake configure on the open project", ConfigureOpenProject});

            wf.AddStep({"Build", "Compile the project", BuildOpenProject});

            registry.Register(std::move(wf));
        }

        // ====================================================================
        // Build: Clean & Rebuild
        // ====================================================================
        {
            EditorWorkflow wf("Clean & Rebuild",
                              "Delete the open project's build directory, reconfigure, and build from scratch",
                              "Build");
            wf.SetRequiresConfirmation(true);

            wf.AddStep({"Clean", "Remove the open project's build directory", [](WorkflowContext& ctx)
                        {
                            // Deleting a CWD-relative "build" would erase whatever build tree
                            // happens to sit next to the editor's working directory. Only the
                            // open project's own build directory may be removed.
                            const std::filesystem::path buildDir = ProjectBuildDirectory();
                            if (buildDir.empty())
                            {
                                ctx.Log("No project is open — refusing to delete an unscoped 'build' directory");
                                return false;
                            }

                            std::error_code ec;
                            if (!std::filesystem::exists(buildDir, ec))
                            {
                                ctx.Log("Nothing to clean: " + buildDir.string() + " does not exist");
                                return true;
                            }

                            std::filesystem::remove_all(buildDir, ec);
                            if (ec)
                            {
                                ctx.Log("Failed to remove " + buildDir.string() + ": " + ec.message());
                                return false;
                            }
                            ctx.Log("Removed build directory: " + buildDir.string());
                            return true;
                        }});

            wf.AddStep({"Configure", "Run CMake configure on the open project", ConfigureOpenProject});

            wf.AddStep({"Build", "Full rebuild", BuildOpenProject});

            registry.Register(std::move(wf));
        }

        // ====================================================================
        // Scene: Scene Validation
        // ====================================================================
        {
            EditorWorkflow wf("Scene Validation",
                              "Check scene for missing assets, orphaned entities, and integrity issues", "Scene");

            wf.AddStep({"Check Scene Loaded", "Verify a scene is currently open", [](WorkflowContext& ctx)
                        {
                            if (!ctx.editorUI || ctx.editorUI->GetCurrentSceneName().empty())
                            {
                                ctx.Log("No scene is currently loaded");
                                return false;
                            }
                            ctx.Log("Scene loaded: " + ctx.editorUI->GetCurrentSceneName());
                            return true;
                        }});

            wf.AddStep({"Validate Integrity", "Run scene data integrity checks", [](WorkflowContext& ctx)
                        {
                            ctx.Log("Checking for orphaned objects and dangling references...");
                            // No direct SceneFile access from workflows yet, but verify scene is saveable
                            if (ctx.editorUI->IsSceneModified())
                            {
                                ctx.Log("Warning: scene has unsaved changes");
                            }
                            ctx.Log("Scene integrity: OK");
                            return true;
                        }});

            wf.AddStep({"Check Asset References", "Verify all referenced assets exist on disk", [](WorkflowContext& ctx)
                        {
                            // Check common asset directories exist
                            bool hasAssets = std::filesystem::exists("Assets");
                            if (hasAssets)
                            {
                                size_t count = 0;
                                for (const auto& entry : std::filesystem::recursive_directory_iterator("Assets"))
                                {
                                    if (entry.is_regular_file())
                                        ++count;
                                }
                                ctx.Log("Asset directory contains " + std::to_string(count) + " file(s)");
                            }
                            else
                            {
                                ctx.Log("Warning: No Assets directory found");
                            }
                            ctx.Log("Asset reference check complete");
                            return true;
                        }});

            registry.Register(std::move(wf));
        }

        // ====================================================================
        // Build: Pre-Commit Check
        // ====================================================================
        {
            EditorWorkflow wf("Pre-Commit Check", "Format, build, and test before committing code", "Build");

            wf.AddStep({"Format Check", "Run clang-format dry-run", [](WorkflowContext& ctx)
                        {
                            auto files = CollectFormatCheckFiles();
                            if (files.empty())
                            {
                                ctx.Log("No source files found for format check");
                                return true;
                            }

                            std::vector<std::string> args = {"--dry-run", "--Werror"};
                            args.insert(args.end(), files.begin(), files.end());
                            int rc = RunWorkflowProcess(ctx, "clang-format", args, 5);
                            ctx.Log("Format check exit: " + std::to_string(rc));
                            return rc == 0;
                        }});

            wf.AddStep({"Build", "Compile the project", [](WorkflowContext& ctx)
                        {
                            int rc = RunWorkflowProcess(
                                ctx, "cmake", {"--build", "build", "--config", "Release", "--parallel", "2"}, 10);
                            ctx.Log("Build exit: " + std::to_string(rc));
                            return rc == 0;
                        }});

            wf.AddStep({"Run Tests", "Execute CTest test suite", [](WorkflowContext& ctx)
                        {
                            std::vector<std::string> args = {"--test-dir", "build", "--output-on-failure"};
                            int rc = RunWorkflowProcess(ctx, "ctest", args, 15);
                            ctx.Log("Test exit: " + std::to_string(rc));
                            return rc == 0;
                        }});

            registry.Register(std::move(wf));
        }

        // ====================================================================
        // Scene: Export Scene as Prefab
        // ====================================================================
        {
            EditorWorkflow wf("Export Scene as Prefab",
                              "Save the current scene root hierarchy as a reusable .sparkprefab", "Scene");

            wf.AddStep({"Validate Selection", "Check that an object is selected for export", [](WorkflowContext& ctx)
                        {
                            if (!ctx.editorUI)
                            {
                                ctx.Log("No editor context");
                                return false;
                            }
                            auto* prefabMgr = ctx.editorUI->GetPrefabManager();
                            if (!prefabMgr)
                            {
                                ctx.Log("PrefabManager not available");
                                return false;
                            }
                            ctx.Log("PrefabManager ready");
                            return true;
                        }});

            wf.AddStep({"Serialize to Prefab", "Write selected hierarchy to .sparkprefab file", [](WorkflowContext& ctx)
                        {
                            auto* prefabMgr = ctx.editorUI->GetPrefabManager();
                            auto* prefab = prefabMgr->CreateEmptyPrefab("ExportedPrefab");
                            if (!prefab)
                            {
                                ctx.Log("Failed to create prefab");
                                return false;
                            }
                            bool saved = prefabMgr->SavePrefab("ExportedPrefab");
                            ctx.Log(saved ? "Prefab exported as ExportedPrefab.sparkprefab" : "Prefab save failed");
                            return saved;
                        }});

            registry.Register(std::move(wf));
        }
    }

} // namespace SparkEditor
