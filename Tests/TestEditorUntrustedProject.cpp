/**
 * @file TestEditorUntrustedProject.cpp
 * @brief SEC2: an opened project is untrusted input to the editor.
 *
 * Covers two editor trust boundaries driven by project contents:
 *  - BuildPipeline cook/package must not follow links planted in the project
 *    (content links would copy the developer's files into distributable output;
 *    a linked "Build" directory would redirect writes outside the project);
 *  - ProjectManager must reject an oversized project document (or backup)
 *    before reading it into memory.
 */

#include "TestFramework.h"
#include "Fixtures/ScopedEditorProfile.h"
#include "Core/ProjectManager.h"
#include "Panels/BuildPipeline.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>

using namespace SparkEditor;

namespace
{
    namespace fs = std::filesystem;

    class Scratch
    {
      public:
        explicit Scratch(const char* tag)
        {
            static std::atomic<unsigned int> sequence{0};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            m_root = fs::temp_directory_path() / ("spark-untrusted-" + std::string(tag) + "-" + std::to_string(stamp) +
                                                  "-" + std::to_string(sequence++));
            fs::create_directories(m_root);
        }
        ~Scratch()
        {
            std::error_code ec;
            fs::remove_all(m_root, ec);
        }
        Scratch(const Scratch&) = delete;
        Scratch& operator=(const Scratch&) = delete;

        const fs::path& Root() const { return m_root; }

      private:
        fs::path m_root;
    };

    void WriteBytes(const fs::path& path, const std::string& bytes)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << bytes;
    }

    std::string Utf8(const fs::path& path)
    {
        const std::u8string utf8 = path.u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }

    // Directory link an unprivileged test can create: a symlink where allowed,
    // otherwise (Windows without Developer Mode) an NTFS junction.
    bool MakeDirectoryLink(const fs::path& target, const fs::path& link)
    {
        std::error_code error;
        fs::create_directory_symlink(target, link, error);
        if (!error)
            return true;
#if defined(_WIN32)
        const std::wstring command =
            L"cmd /c mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\" >nul 2>&1";
        return _wsystem(command.c_str()) == 0 && fs::exists(fs::symlink_status(link, error));
#else
        return false;
#endif
    }

    // A cookable project: Assets, Scenes, Config and a project document.
    fs::path MakeProject(const fs::path& parent)
    {
        const fs::path project = parent / "Project";
        fs::create_directories(project / "Assets");
        fs::create_directories(project / "Scenes");
        fs::create_directories(project / "Config");
        WriteBytes(project / "Assets" / "asset.txt", "asset");
        WriteBytes(project / "Scenes" / "Default.sparkscene", "{}");
        WriteBytes(project / "Config" / "EditorSettings.json", "{}");
        WriteBytes(project / "Project.sparkproject", "{}");
        return project;
    }

    BuildResult Cook(const fs::path& project, const std::string& outputDirectory)
    {
        BuildCookPanel::BuildSettings settings;
        settings.outputDirectory = outputDirectory;
        settings.cookAssets = false; // content copy only; no SparkCooker dependency
        BuildPipeline pipeline;
        if (!pipeline.StartCookOnly(settings, project.string()))
            return BuildResult::Failed;
        for (int i = 0; i < 1000 && pipeline.IsRunning(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return pipeline.IsRunning() ? BuildResult::None : pipeline.GetResult();
    }

    constexpr const char* kProjectDocument = R"({
  "projectFileVersion": 1,
  "name": "Bounded",
  "version": "1.0.0",
  "description": "SEC2 size-cap fixture",
  "engineVersion": "0.9.0",
  "defaultScene": "Scenes/Main.sparkscene",
  "modules": ["Bounded"],
  "scenes": ["Scenes/Main.sparkscene"]
})";

    // A valid project document padded with JSON whitespace to exactly totalBytes.
    std::string PaddedProjectDocument(uint64_t totalBytes)
    {
        const std::string document = kProjectDocument;
        std::string padded = "{";
        padded.append(static_cast<size_t>(totalBytes - document.size()), ' ');
        padded.append(document.substr(1));
        return padded;
    }
} // namespace

// ============================================================================
// BuildPipeline: links planted in a project
// ============================================================================

TEST(UntrustedProject_CookRefusesLinkedProjectContent)
{
    Scratch scratch("content-link");
    const fs::path project = MakeProject(scratch.Root());
    const fs::path outside = scratch.Root() / "Outside";
    fs::create_directories(outside);
    WriteBytes(outside / "secret.txt", "developer secret");

    // Control: the same project without a link cooks successfully.
    ASSERT_TRUE(Cook(project, "CleanCook") == BuildResult::Success);
    EXPECT_TRUE(fs::is_regular_file(project / "CleanCook" / "Config" / "EditorSettings.json"));

    // A file link is the disclosure case; fall back to a directory link or junction
    // where the host does not allow unprivileged file symlinks.
    std::error_code linkError;
    fs::create_symlink(outside / "secret.txt", project / "Config" / "secret.txt", linkError);
    if (linkError && !MakeDirectoryLink(outside, project / "Config" / "Linked"))
        SKIP_TEST("cannot create a symlink or junction on this host");

    EXPECT_TRUE(Cook(project, "LinkedCook") == BuildResult::Failed);
    EXPECT_FALSE(fs::exists(project / "LinkedCook" / "Config" / "secret.txt"));
    EXPECT_FALSE(fs::exists(project / "LinkedCook" / "Config" / "Linked" / "secret.txt"));
}

TEST(UntrustedProject_CookRefusesOutputThroughProjectLink)
{
    Scratch scratch("output-link");
    const fs::path project = MakeProject(scratch.Root());
    const fs::path outside = scratch.Root() / "Elsewhere";
    fs::create_directories(outside);

    // The default cook output parent ("Build") shipped as a link out of the project.
    if (!MakeDirectoryLink(outside, project / "Build"))
        SKIP_TEST("cannot create a directory symlink or junction on this host");
    EXPECT_TRUE(Cook(project, "Build/Output") == BuildResult::Failed);
    EXPECT_FALSE(fs::exists(outside / "Output"));

    // A link back into packaged content is refused too, not written into.
    ASSERT_TRUE(MakeDirectoryLink(project / "Scenes", project / "Staging"));
    EXPECT_TRUE(Cook(project, "Staging/Output") == BuildResult::Failed);
    EXPECT_FALSE(fs::exists(project / "Scenes" / "Output"));

    // Control: a real output directory next to the links still cooks.
    EXPECT_TRUE(Cook(project, "RealOutput") == BuildResult::Success);
}

TEST(UntrustedProject_PackageRefusesOutputThroughProjectLink)
{
    Scratch scratch("package-link");
    const fs::path project = MakeProject(scratch.Root());
    const fs::path outside = scratch.Root() / "Elsewhere";
    fs::create_directories(outside);
    if (!MakeDirectoryLink(outside, project / "Build"))
        SKIP_TEST("cannot create a directory symlink or junction on this host");

    BuildCookPanel::BuildSettings settings;
    settings.platform = BuildPipeline::NativeTargetPlatform();
    settings.executableName = "Game";
    std::string error;
    // The output check runs before any runtime artifact is inspected or staged.
    EXPECT_FALSE(BuildPipeline::AssembleNativePackage(settings, project.string(), "missing-host", "missing-module",
                                                      (project / "Build" / "Output").string(), &error));
    EXPECT_STR_CONTAINS(error, "link");
    EXPECT_TRUE(fs::is_empty(outside));
}

// ============================================================================
// ProjectManager: project document size cap
// ============================================================================

TEST(UntrustedProject_OversizedProjectDocumentIsRejectedBeforeRead)
{
    Scratch scratch("oversized-doc");
    const fs::path directory = scratch.Root() / "Bounded";
    fs::create_directories(directory);
    const fs::path document = directory / "Bounded.sparkproject";
    const std::string cap = std::to_string(ProjectManager::kMaximumProjectDocumentBytes);

    // Control: a valid document just under the cap opens (the padding is valid JSON).
    WriteBytes(document, PaddedProjectDocument(ProjectManager::kMaximumProjectDocumentBytes - 16));
    {
        SparkEditor::Testing::IsolatedProjectManager manager;
        manager.Initialize();
        std::string error;
        ASSERT_TRUE(manager.OpenProject(Utf8(document), &error));
        EXPECT_EQ(manager.GetCurrentProject().name, std::string("Bounded"));
        manager.CloseProject();
    }
    std::error_code ec;
    fs::remove(fs::path(document.native() + fs::path(".bak").native()), ec);

    // The same valid document one byte over the cap is refused on its size alone.
    WriteBytes(document, PaddedProjectDocument(ProjectManager::kMaximumProjectDocumentBytes + 1));
    SparkEditor::Testing::IsolatedProjectManager manager;
    manager.Initialize();
    std::string error;
    EXPECT_FALSE(manager.OpenProject(Utf8(document), &error));
    EXPECT_FALSE(manager.HasOpenProject());
    EXPECT_STR_CONTAINS(error, "exceeds " + cap + " bytes");
}

TEST(UntrustedProject_OversizedProjectBackupIsRejected)
{
    Scratch scratch("oversized-bak");
    const fs::path directory = scratch.Root() / "Bounded";
    fs::create_directories(directory);
    const fs::path document = directory / "Bounded.sparkproject";
    const fs::path backup = fs::path(document.native() + fs::path(".bak").native());
    const std::string cap = std::to_string(ProjectManager::kMaximumProjectDocumentBytes);

    // A torn primary sends the loader to the previous-good backup, which is valid
    // JSON but over the cap: it must be refused rather than read.
    const std::string valid = kProjectDocument;
    WriteBytes(document, valid.substr(0, valid.size() / 2));
    WriteBytes(backup, PaddedProjectDocument(ProjectManager::kMaximumProjectDocumentBytes + 1));

    SparkEditor::Testing::IsolatedProjectManager manager;
    manager.Initialize();
    std::string error;
    EXPECT_FALSE(manager.OpenProject(Utf8(document), &error));
    EXPECT_STR_CONTAINS(error, "was not usable: the file exceeds " + cap + " bytes");

    // Control: the same backup within the cap is recovered.
    WriteBytes(backup, kProjectDocument);
    error.clear();
    EXPECT_TRUE(manager.OpenProject(Utf8(document), &error));
    EXPECT_STR_CONTAINS(error, "Loaded the previous-good backup");
}
