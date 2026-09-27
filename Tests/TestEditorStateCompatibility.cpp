/**
 * @file TestEditorStateCompatibility.cpp
 * @brief SAVE-230: .sparkproject version window, durable save and backup recovery.
 *
 * Drives the production ProjectManager against the committed fixtures in
 * Tests/Fixtures/Compatibility/EditorState (see the README there). Each fixture is copied
 * into a scratch project directory first, because opening a project adds missing build
 * scaffold files next to its document.
 */

#include "TestFramework.h"
#include "Fixtures/ScopedEditorProfile.h"
#include "Core/ProjectManager.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

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
