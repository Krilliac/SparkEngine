/**
 * @file TestEditorStateCompatibility.cpp
 * @brief SAVE-230: .sparkproject version window, durable save and backup recovery.
 *
 * Drives the production ProjectManager against the committed fixtures in
 * Tests/Fixtures/Compatibility/EditorState (see the README there). Each fixture is copied
 * into a scratch project directory first, because opening a project adds missing build
 * scaffold files next to its document. EditorLayoutManager reads the committed layout
 * fixtures in Layouts/ in place, because loading a layout never writes.
 */

#include "TestFramework.h"
#include "Fixtures/ScopedEditorProfile.h"
#include "Core/EditorLayoutManager.h"
#include "Core/ProjectManager.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace
{
    namespace fs = std::filesystem;

    fs::path EditorStateFixture(const char* relative)
    {
#if defined(_WIN32) && defined(SPARK_TEST_SOURCE_DIR_WIDE)
        const fs::path sourceDir(SPARK_TEST_SOURCE_DIR_WIDE);
#else
        const fs::path sourceDir(SPARK_TEST_SOURCE_DIR);
#endif
        return sourceDir / "Tests" / "Fixtures" / "Compatibility" / "EditorState" / relative;
    }

    /// ProjectManager takes UTF-8; path::string() is the ANSI code page on Windows.
    std::string Utf8(const fs::path& path)
    {
        const std::u8string utf8 = path.u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }

    std::string ReadBytes(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    void WriteBytes(const fs::path& path, const std::string& bytes)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << bytes;
    }

    fs::path WithSuffix(fs::path path, const char* suffix)
    {
        path += suffix;
        return path;
    }

#if defined(_WIN32)
    /// Holds @p path open without FILE_SHARE_DELETE, so MoveFileExW cannot replace it while
    /// reads (and the .bak refresh's copy) still succeed.
    class RenameBlocker
    {
      public:
        explicit RenameBlocker(const fs::path& path)
            : m_file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr))
        {
        }
        ~RenameBlocker()
        {
            if (m_file != INVALID_HANDLE_VALUE)
                ::CloseHandle(m_file);
        }
        RenameBlocker(const RenameBlocker&) = delete;
        RenameBlocker& operator=(const RenameBlocker&) = delete;

        bool Held() const { return m_file != INVALID_HANDLE_VALUE; }

      private:
        HANDLE m_file;
    };
#endif

    class ProjectScratch
    {
      public:
        explicit ProjectScratch(const char* tag)
        {
            static std::atomic<unsigned int> sequence{0};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            m_root = fs::temp_directory_path() / ("spark-editor-state-" + std::string(tag) + "-" +
                                                  std::to_string(stamp) + "-" + std::to_string(sequence++));
            fs::create_directories(m_root);
        }
        ~ProjectScratch()
        {
            std::error_code ec;
            fs::remove_all(m_root, ec);
        }
        ProjectScratch(const ProjectScratch&) = delete;
        ProjectScratch& operator=(const ProjectScratch&) = delete;

        /// Copy a committed fixture document into its own scratch project directory.
        fs::path Stage(const char* fixture, const char* projectName) const
        {
            const fs::path directory = m_root / projectName;
            fs::create_directories(directory);
            const fs::path document = directory / (std::string(projectName) + ".sparkproject");
            fs::copy_file(EditorStateFixture(fixture), document, fs::copy_options::overwrite_existing);
            return document;
        }

        /// An empty scratch directory, for documents that are not projects.
        fs::path Directory(const char* name) const
        {
            const fs::path directory = m_root / name;
            fs::create_directories(directory);
            return directory;
        }

      private:
        fs::path m_root;
    };
} // namespace

TEST(EditorStateMigration_V1ProjectFixtureLoadsDeclaredStateWithoutRewrite)
{
    const std::string committed = ReadBytes(EditorStateFixture("v1-project/V1Project.sparkproject"));
    ASSERT_FALSE(committed.empty());

    ProjectScratch scratch("v1");
    const fs::path document = scratch.Stage("v1-project/V1Project.sparkproject", "V1Project");

    SparkEditor::Testing::IsolatedProjectManager manager;
    manager.Initialize();
    std::string error;
    ASSERT_TRUE(manager.OpenProject(Utf8(document), &error));
    EXPECT_TRUE(error.empty());

    const SparkEditor::ProjectInfo& project = manager.GetCurrentProject();
    EXPECT_EQ(project.name, std::string("V1Project"));
    EXPECT_EQ(project.version, std::string("2.3.4"));
    EXPECT_EQ(project.description, std::string("SAVE-230 editor-state fixture"));
    EXPECT_EQ(project.engineVersion, std::string("0.9.0"));
    EXPECT_TRUE(project.hasTemplateIdentity);
    EXPECT_TRUE(project.templateType == SparkEditor::ProjectTemplate::FirstPerson);
    EXPECT_EQ(project.defaultScene, std::string("Scenes/Main.sparkscene"));
    EXPECT_EQ(project.lastOpenedScene, std::string("Scenes/Arena.sparkscene"));
    EXPECT_EQ(project.createdTime, static_cast<uint64_t>(1767225600));
    EXPECT_EQ(project.lastModified, static_cast<uint64_t>(1767312000));
    EXPECT_TRUE(project.modules == (std::vector<std::string>{"V1Project", "SharedGameplay"}));
    EXPECT_TRUE(project.scenes == (std::vector<std::string>{"Scenes/Main.sparkscene", "Scenes/Arena.sparkscene"}));

    // Opening is read-only for the document: no rewrite, no staging or backup sibling.
    EXPECT_TRUE(ReadBytes(document) == committed);
    EXPECT_FALSE(fs::exists(WithSuffix(document, ".tmp")));
    EXPECT_FALSE(fs::exists(WithSuffix(document, ".bak")));
    manager.RemoveRecentProject(Utf8(document));
}

TEST(EditorStateMigration_FutureProjectVersionFailsClosedWithVersionedError)
{
    ProjectScratch scratch("future");
    const fs::path current = scratch.Stage("v1-project/V1Project.sparkproject", "V1Project");
    const fs::path future = scratch.Stage("v2-future-project/Future.sparkproject", "Future");
    const std::string futureBytes = ReadBytes(future);
    // A loadable retained copy must not be used for a newer document.
    fs::copy_file(current, WithSuffix(future, ".bak"));

    SparkEditor::Testing::IsolatedProjectManager manager;
    manager.Initialize();
    ASSERT_TRUE(manager.OpenProject(Utf8(current)));

    std::string error;
    EXPECT_FALSE(manager.OpenProject(Utf8(future), &error));
    EXPECT_STR_CONTAINS(error, "Future.sparkproject");
    EXPECT_STR_CONTAINS(error, "projectFileVersion 2");
    EXPECT_STR_CONTAINS(error, "reads 1 and writes 1");
    EXPECT_STR_CONTAINS(error, "newer SparkEditor");
    EXPECT_TRUE(error.find(".bak") == std::string::npos);

    // The previously open project is untouched, and the newer document is not rewritten.
    EXPECT_TRUE(manager.HasOpenProject());
    EXPECT_EQ(manager.GetCurrentProject().name, std::string("V1Project"));
    EXPECT_TRUE(manager.GetCurrentProject().modules == (std::vector<std::string>{"V1Project", "SharedGameplay"}));
    EXPECT_TRUE(ReadBytes(future) == futureBytes);
    manager.RemoveRecentProject(Utf8(current));
}

TEST(EditorStateMigration_FailedProjectSaveKeepsPreviousFileAndBackup)
{
    ProjectScratch scratch("failedsave");
    const fs::path document = scratch.Stage("v1-project/V1Project.sparkproject", "V1Project");
    const std::string original = ReadBytes(document);

    SparkEditor::Testing::IsolatedProjectManager manager;
    manager.Initialize();
    ASSERT_TRUE(manager.OpenProject(Utf8(document)));
    ASSERT_TRUE(manager.SaveProject());
    // The save kept the opened document as the previous-good copy.
    EXPECT_TRUE(ReadBytes(WithSuffix(document, ".bak")) == original);
    const std::string savedPrimary = ReadBytes(document);
    const std::string savedBackup = ReadBytes(WithSuffix(document, ".bak"));
    EXPECT_STR_CONTAINS(savedPrimary, "\"projectFileVersion\": 1,");

    // A non-empty directory on the staging name makes the next save fail before any rename.
    const fs::path staging = WithSuffix(document, ".tmp");
    fs::create_directories(staging);
    WriteBytes(staging / "occupant", "x");

    EXPECT_FALSE(manager.SaveProject());
    EXPECT_TRUE(ReadBytes(document) == savedPrimary);
    EXPECT_TRUE(ReadBytes(WithSuffix(document, ".bak")) == savedBackup);
    manager.RemoveRecentProject(Utf8(document));
}

TEST(EditorStateMigration_CorruptProjectLoadsRetainedBackup)
{
    ProjectScratch scratch("recover");
    const fs::path document = scratch.Stage("v1-project/V1Project.sparkproject", "V1Project");

    {
        SparkEditor::Testing::IsolatedProjectManager writer;
        writer.Initialize();
        ASSERT_TRUE(writer.OpenProject(Utf8(document)));
        ASSERT_TRUE(writer.SaveProject());
        writer.RemoveRecentProject(Utf8(document));
    }
    ASSERT_TRUE(fs::exists(WithSuffix(document, ".bak")));

    // A torn external copy: the document stops mid-object.
    const std::string saved = ReadBytes(document);
    WriteBytes(document, saved.substr(0, saved.size() / 2));

    SparkEditor::Testing::IsolatedProjectManager manager;
    manager.Initialize();
    std::string error;
    ASSERT_TRUE(manager.OpenProject(Utf8(document), &error));
    EXPECT_EQ(manager.GetCurrentProject().name, std::string("V1Project"));
    EXPECT_EQ(manager.GetCurrentProject().defaultScene, std::string("Scenes/Main.sparkscene"));
    EXPECT_TRUE(manager.GetCurrentProject().modules == (std::vector<std::string>{"V1Project", "SharedGameplay"}));
    EXPECT_STR_CONTAINS(error, "not a complete JSON object");
    EXPECT_STR_CONTAINS(error, "Loaded the previous-good backup");
    manager.RemoveRecentProject(Utf8(document));
    manager.CloseProject();

    // With the backup damaged too, the open fails and names both reasons.
    WriteBytes(WithSuffix(document, ".bak"), "");
    EXPECT_FALSE(manager.OpenProject(Utf8(document), &error));
    EXPECT_FALSE(manager.HasOpenProject());
    EXPECT_STR_CONTAINS(error, "was rejected: the file is not a complete JSON object");
    EXPECT_STR_CONTAINS(error, "V1Project.sparkproject.bak' was not usable: the file is empty");
}

TEST(EditorStateMigration_SaveAfterBackupRecoveryKeepsGoodBackup)
{
    ProjectScratch scratch("recoversave");
    const fs::path document = scratch.Stage("v1-project/V1Project.sparkproject", "V1Project");
    const fs::path backup = WithSuffix(document, ".bak");

    {
        SparkEditor::Testing::IsolatedProjectManager writer;
        writer.Initialize();
        ASSERT_TRUE(writer.OpenProject(Utf8(document)));
        ASSERT_TRUE(writer.SaveProject());
        writer.RemoveRecentProject(Utf8(document));
    }
    const std::string goodBackup = ReadBytes(backup);
    ASSERT_FALSE(goodBackup.empty());

    const std::string saved = ReadBytes(document);
    const std::string damaged = saved.substr(0, saved.size() / 2);
    WriteBytes(document, damaged);

    SparkEditor::Testing::IsolatedProjectManager manager;
    manager.Initialize();
    std::string error;
    ASSERT_TRUE(manager.OpenProject(Utf8(document), &error));
    EXPECT_STR_CONTAINS(error, "Loaded the previous-good backup");

#if defined(_WIN32)
    {
        // The final rename fails after the point where the .bak would be refreshed.
        RenameBlocker blocker(document);
        ASSERT_TRUE(blocker.Held());
        EXPECT_FALSE(manager.SaveProject());
    }
    EXPECT_TRUE(ReadBytes(document) == damaged);
    EXPECT_TRUE(ReadBytes(backup) == goodBackup);
#endif

    // The repairing save leaves the .bak on the last good document, not the damaged one.
    ASSERT_TRUE(manager.SaveProject());
    EXPECT_TRUE(ReadBytes(backup) == goodBackup);
    const std::string repaired = ReadBytes(document);
    EXPECT_STR_CONTAINS(repaired, "\"projectFileVersion\": 1,");

    // Once the document is good again, saves retain it as usual.
    ASSERT_TRUE(manager.SaveProject());
    EXPECT_TRUE(ReadBytes(backup) == repaired);
    manager.RemoveRecentProject(Utf8(document));
}

namespace
{
    SparkEditor::PanelConfig RegisteredPanel(const char* name)
    {
        SparkEditor::PanelConfig panel;
        panel.name = name;
        panel.displayName = name;
        panel.sizeX = 111.0f;
        panel.sizeY = 222.0f;
        return panel;
    }

    /// EditorLayoutManager takes the directory as the editor passes it (path::string()).
    bool InitializeOnLayoutFixtures(SparkEditor::EditorLayoutManager& layouts)
    {
        layouts.RegisterPanel(RegisteredPanel("Hierarchy"));
        layouts.RegisterPanel(RegisteredPanel("Inspector"));
        layouts.RegisterPanel(RegisteredPanel("Console"));
        return layouts.Initialize(EditorStateFixture("Layouts").string());
    }
} // namespace

TEST(EditorStateMigration_V1LayoutFixtureAppliesDeclaredPanels)
{
    SparkEditor::EditorLayoutManager layouts;
    ASSERT_TRUE(InitializeOnLayoutFixtures(layouts));
    ASSERT_TRUE(layouts.LoadLayout("v1-layout"));
    EXPECT_TRUE(layouts.GetLastError().empty());
    EXPECT_EQ(layouts.GetCurrentLayoutName(), std::string("v1-layout"));

    const SparkEditor::PanelConfig* hierarchy = layouts.GetPanelConfig("Hierarchy");
    ASSERT_TRUE(hierarchy != nullptr);
    EXPECT_EQ(hierarchy->displayName, std::string("Scene Hierarchy"));
    EXPECT_TRUE(hierarchy->dockPosition == SparkEditor::LayoutDockPosition::Left);
    EXPECT_EQ(hierarchy->sizeX, 320.0f);
    EXPECT_EQ(hierarchy->sizeY, 640.0f);
    EXPECT_EQ(hierarchy->posY, 48.0f);
    EXPECT_TRUE(hierarchy->isVisible);
    EXPECT_EQ(hierarchy->dockRatio, 0.25f);
    EXPECT_EQ(hierarchy->parentDock, std::string("MainDock"));

    const SparkEditor::PanelConfig* inspector = layouts.GetPanelConfig("Inspector");
    ASSERT_TRUE(inspector != nullptr);
    EXPECT_TRUE(inspector->dockPosition == SparkEditor::LayoutDockPosition::Right);
    EXPECT_EQ(inspector->sizeX, 360.5f);
    EXPECT_EQ(inspector->posX, 1520.0f);
    EXPECT_FALSE(inspector->isVisible);
    EXPECT_EQ(inspector->dockRatio, 0.1875f);
    EXPECT_EQ(inspector->tabOrder, 1);

    const SparkEditor::PanelConfig* console = layouts.GetPanelConfig("Console");
    ASSERT_TRUE(console != nullptr);
    EXPECT_TRUE(console->dockPosition == SparkEditor::LayoutDockPosition::Bottom);
    EXPECT_TRUE(console->isFloating);
    EXPECT_FALSE(console->canClose);
    EXPECT_FALSE(console->canDock);
    EXPECT_EQ(console->tabOrder, 2);
    EXPECT_TRUE(console->parentDock.empty());

    // The legacy dialect, without a "version" key, is version 1 and loads the same way.
    std::string legacy = ReadBytes(EditorStateFixture("Layouts/v1-layout.json"));
    const size_t versionLine = legacy.find("\"version\"");
    ASSERT_TRUE(versionLine != std::string::npos);
    legacy.erase(versionLine, legacy.find('\n', versionLine) + 1 - versionLine);
    ASSERT_TRUE(legacy.find("\"version\"") == std::string::npos);
    ProjectScratch scratch("legacy-layout");
    const fs::path legacyDirectory = scratch.Directory("Layouts");
    WriteBytes(legacyDirectory / "legacy.json", legacy);

    SparkEditor::EditorLayoutManager legacyLayouts;
    ASSERT_TRUE(legacyLayouts.Initialize(legacyDirectory.string()));
    legacyLayouts.RegisterPanel(RegisteredPanel("Inspector"));
    ASSERT_TRUE(legacyLayouts.LoadLayout("legacy"));
    EXPECT_EQ(legacyLayouts.GetPanelConfig("Inspector")->sizeX, 360.5f);
}

TEST(EditorStateMigration_FutureLayoutVersionFailsClosedWithVersionedError)
{
    SparkEditor::EditorLayoutManager layouts;
    ASSERT_TRUE(InitializeOnLayoutFixtures(layouts));
    ASSERT_TRUE(layouts.LoadLayout("v1-layout"));

    EXPECT_FALSE(layouts.LoadLayout("v2-future-layout"));
    const std::string& error = layouts.GetLastError();
    EXPECT_STR_CONTAINS(error, "v2-future-layout.json");
    EXPECT_STR_CONTAINS(error, "layout format version 2");
    EXPECT_STR_CONTAINS(error, "reads layout version 1 only");
    EXPECT_STR_CONTAINS(error, "newer SparkEditor");

    // Nothing from the newer file was applied, and the current layout is still the v1 one.
    const SparkEditor::PanelConfig* hierarchy = layouts.GetPanelConfig("Hierarchy");
    ASSERT_TRUE(hierarchy != nullptr);
    EXPECT_EQ(hierarchy->sizeX, 320.0f);
    EXPECT_TRUE(hierarchy->isVisible);
    EXPECT_EQ(hierarchy->parentDock, std::string("MainDock"));
    EXPECT_EQ(layouts.GetCurrentLayoutName(), std::string("v1-layout"));
}

TEST(EditorStateMigration_LayoutFixtureIsNotRewrittenOnLoad)
{
    const fs::path v1 = EditorStateFixture("Layouts/v1-layout.json");
    const fs::path future = EditorStateFixture("Layouts/v2-future-layout.json");
    const std::string v1Bytes = ReadBytes(v1);
    const std::string futureBytes = ReadBytes(future);
    ASSERT_FALSE(v1Bytes.empty());
    ASSERT_FALSE(futureBytes.empty());

    SparkEditor::EditorLayoutManager layouts;
    ASSERT_TRUE(InitializeOnLayoutFixtures(layouts));
    EXPECT_TRUE(layouts.LoadLayout("v1-layout"));
    EXPECT_FALSE(layouts.LoadLayout("v2-future-layout"));

    // Loading, accepted or rejected, never writes: no rewrite and no staging sibling.
    EXPECT_TRUE(ReadBytes(v1) == v1Bytes);
    EXPECT_TRUE(ReadBytes(future) == futureBytes);
    EXPECT_FALSE(fs::exists(WithSuffix(v1, ".tmp")));
    EXPECT_FALSE(fs::exists(WithSuffix(future, ".tmp")));

    const std::vector<SparkEditor::LayoutInfo> listed = layouts.GetSavedLayouts();
    ASSERT_EQ(listed.size(), static_cast<size_t>(2));
    EXPECT_EQ(listed[0].name, std::string("v1-layout"));
    EXPECT_EQ(listed[0].description, std::string("Three-panel authoring layout"));
    EXPECT_EQ(listed[1].name, std::string("v2-future-layout"));
}
