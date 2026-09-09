// TestEditorRecovery.cpp - Durable editor recovery persistence contracts.

#include "TestFramework.h"

#include "Core/EditorRecovery.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    namespace fs = std::filesystem;

    class ScopedRecoveryDirectory
    {
      public:
        ScopedRecoveryDirectory()
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            m_path = fs::temp_directory_path() / ("spark-editor-recovery-" + std::to_string(stamp));
            std::error_code error;
            fs::remove_all(m_path, error);
            fs::create_directories(m_path, error);
        }

        ~ScopedRecoveryDirectory()
        {
            std::error_code error;
            fs::remove_all(m_path, error);
        }

        const fs::path& Path() const { return m_path; }

      private:
        fs::path m_path;
    };

    std::string ReadAll(const fs::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    bool WriteAll(const fs::path& path, const std::string& contents)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.close();
        return output.good();
    }
} // namespace

TEST(EditorRecovery_QuotedWorldSnapshotRoundTripsThroughPrimary)
{
    ScopedRecoveryDirectory scratch;
    SparkEditor::EditorRecoveryStore store(scratch.Path());

    SparkEditor::EditorRecoverySnapshot snapshot;
    snapshot.projectIdentity = "C:/Projects/Recovery Test";
    snapshot.projectRelativeScene = "Scenes/Main.scene";
    snapshot.sceneDisplayName = "Main \"Scene\"";
    snapshot.serializedWorld = R"({"entities":[{"name":"Quoted \"Entity\""}]})";
    snapshot.layoutIniPath = "Layouts/author.ini";
    snapshot.recentOperations = {"Rename \"Entity\""};
    snapshot.dirtySequence = 42;
    snapshot.capturedUnixMilliseconds = 1760000000000;

    std::string error;
    ASSERT_TRUE(store.Save(snapshot, error));

    const SparkEditor::EditorRecoveryLoadResult loaded = store.LoadForProject(snapshot.projectIdentity);
    ASSERT_EQ(static_cast<int>(loaded.state), static_cast<int>(SparkEditor::EditorRecoveryLoadState::Primary));
    ASSERT_TRUE(loaded.snapshot.has_value());
    EXPECT_EQ(loaded.snapshot->sceneDisplayName, std::string("Main \"Scene\""));
    EXPECT_EQ(loaded.snapshot->serializedWorld, snapshot.serializedWorld);
    EXPECT_EQ(loaded.snapshot->recentOperations.size(), size_t(1));
}

TEST(EditorRecovery_InvalidWorldDoesNotReplaceExistingPrimary)
{
    ScopedRecoveryDirectory scratch;
    SparkEditor::EditorRecoveryStore store(scratch.Path());

    SparkEditor::EditorRecoverySnapshot valid;
    valid.projectIdentity = "C:/Projects/Recovery Test";
    valid.projectRelativeScene = "Scenes/Main.scene";
    valid.sceneDisplayName = "Main";
    valid.serializedWorld = R"({"entities":[]})";

    std::string error;
    ASSERT_TRUE(store.Save(valid, error));
    const std::filesystem::path primary = scratch.Path() / "recovery-v1.json";
    const std::string before = ReadAll(primary);
    ASSERT_FALSE(before.empty());

    SparkEditor::EditorRecoverySnapshot invalid = valid;
    invalid.serializedWorld = "not JSON";
    EXPECT_FALSE(store.Save(invalid, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(ReadAll(primary), before);
}

TEST(EditorRecovery_UsesBackupWhenPrimaryIsDamaged)
{
    ScopedRecoveryDirectory scratch;
    SparkEditor::EditorRecoveryStore store(scratch.Path());

    SparkEditor::EditorRecoverySnapshot older;
    older.projectIdentity = "C:/Projects/Recovery Test";
    older.projectRelativeScene = "Scenes/Main.scene";
    older.sceneDisplayName = "Older";
    older.serializedWorld = R"({"entities":[]})";
    older.dirtySequence = 1;

    SparkEditor::EditorRecoverySnapshot newer = older;
    newer.sceneDisplayName = "Newer";
    newer.serializedWorld = R"({"entities":[{"name":"Newer"}]})";
    newer.dirtySequence = 2;

    std::string error;
    ASSERT_TRUE(store.Save(older, error));
    ASSERT_TRUE(store.Save(newer, error));

    const fs::path primary = scratch.Path() / "recovery-v1.json";
    const fs::path backup = scratch.Path() / "recovery-v1.backup.json";
    ASSERT_TRUE(fs::exists(backup));
    ASSERT_TRUE(WriteAll(primary, "{ damaged recovery data"));

    const SparkEditor::EditorRecoveryLoadResult loaded = store.LoadForProject(older.projectIdentity);
    ASSERT_EQ(static_cast<int>(loaded.state), static_cast<int>(SparkEditor::EditorRecoveryLoadState::Backup));
    ASSERT_TRUE(loaded.snapshot.has_value());
    EXPECT_EQ(loaded.snapshot->sceneDisplayName, older.sceneDisplayName);
    EXPECT_EQ(loaded.snapshot->dirtySequence, older.dirtySequence);
}

TEST(EditorRecovery_ClearRemovesPrimaryAndBackup)
{
    ScopedRecoveryDirectory scratch;
    SparkEditor::EditorRecoveryStore store(scratch.Path());

    SparkEditor::EditorRecoverySnapshot snapshot;
    snapshot.projectIdentity = "C:/Projects/Recovery Test";
    snapshot.projectRelativeScene = "Scenes/Main.scene";
    snapshot.sceneDisplayName = "Main";
    snapshot.serializedWorld = R"({"entities":[]})";

    std::string error;
    ASSERT_TRUE(store.Save(snapshot, error));
    snapshot.dirtySequence = 1;
    ASSERT_TRUE(store.Save(snapshot, error));

    ASSERT_TRUE(fs::exists(scratch.Path() / "recovery-v1.json"));
    ASSERT_TRUE(fs::exists(scratch.Path() / "recovery-v1.backup.json"));
    ASSERT_TRUE(store.Clear(error));
    EXPECT_FALSE(fs::exists(scratch.Path() / "recovery-v1.json"));
    EXPECT_FALSE(fs::exists(scratch.Path() / "recovery-v1.backup.json"));
    const SparkEditor::EditorRecoveryLoadResult loaded = store.LoadForProject(snapshot.projectIdentity);
    EXPECT_EQ(static_cast<int>(loaded.state), static_cast<int>(SparkEditor::EditorRecoveryLoadState::None));
}

TEST(EditorRecovery_UnsafeSnapshotsDoNotReplacePrimary)
{
    ScopedRecoveryDirectory scratch;
    SparkEditor::EditorRecoveryStore store(scratch.Path());

    SparkEditor::EditorRecoverySnapshot valid;
    valid.projectIdentity = "C:/Projects/Recovery Test";
    valid.projectRelativeScene = "Scenes/Main.scene";
    valid.sceneDisplayName = "Main";
    valid.serializedWorld = R"({"entities":[]})";

    std::string error;
    ASSERT_TRUE(store.Save(valid, error));
    const fs::path primary = scratch.Path() / "recovery-v1.json";
    const std::string before = ReadAll(primary);

    SparkEditor::EditorRecoverySnapshot unsafeLayout = valid;
    unsafeLayout.layoutIniPath = "../outside.ini";
    EXPECT_FALSE(store.Save(unsafeLayout, error));
    EXPECT_EQ(ReadAll(primary), before);

    SparkEditor::EditorRecoverySnapshot nonObjectWorld = valid;
    nonObjectWorld.serializedWorld = "[]";
    EXPECT_FALSE(store.Save(nonObjectWorld, error));
    EXPECT_EQ(ReadAll(primary), before);
}

TEST(EditorRecovery_EmptyDirectoryIsRejectedWithoutTouchingWorkingDirectory)
{
    SparkEditor::EditorRecoveryStore store(fs::path{});
    SparkEditor::EditorRecoverySnapshot snapshot;
    snapshot.projectIdentity = "C:/Projects/Recovery Test";
    snapshot.sceneDisplayName = "Main";
    snapshot.serializedWorld = R"({"entities":[]})";

    std::string error;
    EXPECT_FALSE(store.Save(snapshot, error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(store.Clear(error));
    EXPECT_FALSE(error.empty());
    const SparkEditor::EditorRecoveryLoadResult loaded = store.LoadForProject(snapshot.projectIdentity);
    EXPECT_EQ(static_cast<int>(loaded.state), static_cast<int>(SparkEditor::EditorRecoveryLoadState::Invalid));
}
