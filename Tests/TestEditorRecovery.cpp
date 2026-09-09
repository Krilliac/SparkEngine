// TestEditorRecovery.cpp - Durable editor recovery persistence contracts.

#include "TestFramework.h"

#include "Core/EditorRecovery.h"
#include "Engine/ECS/Components.h"

#include <array>
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

    fs::path FindRecoveryFileForProject(const fs::path& root, const std::string& projectIdentity,
                                        const std::string& fileName)
    {
        std::error_code error;
        for (fs::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error))
        {
            if (!it->is_regular_file(error) || it->path().filename() != fileName)
                continue;
            if (ReadAll(it->path()).find("\"projectIdentity\": \"" + projectIdentity + "\"") !=
                std::string::npos)
            {
                return it->path();
            }
        }
        return {};
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
    const std::filesystem::path primary = FindRecoveryFileForProject(
        scratch.Path(), valid.projectIdentity, "recovery-v1.json");
    ASSERT_FALSE(primary.empty());
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

    const fs::path primary = FindRecoveryFileForProject(scratch.Path(), older.projectIdentity, "recovery-v1.json");
    const fs::path backup = FindRecoveryFileForProject(
        scratch.Path(), older.projectIdentity, "recovery-v1.backup.json");
    ASSERT_FALSE(primary.empty());
    ASSERT_FALSE(backup.empty());
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

    ASSERT_FALSE(FindRecoveryFileForProject(scratch.Path(), snapshot.projectIdentity, "recovery-v1.json").empty());
    ASSERT_FALSE(
        FindRecoveryFileForProject(scratch.Path(), snapshot.projectIdentity, "recovery-v1.backup.json").empty());
    ASSERT_TRUE(store.Clear(error));
    EXPECT_TRUE(FindRecoveryFileForProject(scratch.Path(), snapshot.projectIdentity, "recovery-v1.json").empty());
    EXPECT_TRUE(
        FindRecoveryFileForProject(scratch.Path(), snapshot.projectIdentity, "recovery-v1.backup.json").empty());
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
    const fs::path primary = FindRecoveryFileForProject(scratch.Path(), valid.projectIdentity, "recovery-v1.json");
    ASSERT_FALSE(primary.empty());
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

TEST(EditorRecovery_CaptureSerializesWorldOwnedByCallingThread)
{
    ::World world;
    const ::EntityID entity = world.CreateEntity("Recovery Entity");
    world.AddComponent<::Transform>(entity);

    SparkEditor::EditorRecoverySnapshot metadata;
    metadata.projectIdentity = "C:/Projects/project-a";
    metadata.projectRelativeScene = "Scenes/Main.sparkscene";
    metadata.sceneDisplayName = "Main";
    metadata.layoutIniPath = "Layouts/author.ini";
    metadata.recentOperations = {"Create Recovery Entity"};
    metadata.dirtySequence = 7;
    metadata.capturedUnixMilliseconds = 1760000000000;

    const SparkEditor::EditorRecoverySnapshot snapshot =
        SparkEditor::CaptureRecoverySnapshotOnCallingThread(world, std::move(metadata));
    EXPECT_TRUE(snapshot.serializedWorld.contains("Recovery Entity"));
    EXPECT_EQ(snapshot.dirtySequence, uint64_t(7));
    EXPECT_EQ(snapshot.recentOperations.size(), size_t(1));
}

TEST(EditorRecovery_ProjectMismatchIsNotOffered)
{
    ScopedRecoveryDirectory scratch;
    SparkEditor::EditorRecoveryStore store(scratch.Path());

    SparkEditor::EditorRecoverySnapshot snapshot;
    snapshot.projectIdentity = "C:/Projects/project-a";
    snapshot.projectRelativeScene = "Scenes/Main.sparkscene";
    snapshot.sceneDisplayName = "Main";
    snapshot.serializedWorld = R"({"entities":[]})";

    std::string error;
    ASSERT_TRUE(store.Save(snapshot, error));
    const SparkEditor::EditorRecoveryLoadResult loaded = store.LoadForProject("C:/Projects/project-b");
    EXPECT_EQ(static_cast<int>(loaded.state), static_cast<int>(SparkEditor::EditorRecoveryLoadState::None));
    EXPECT_FALSE(loaded.snapshot.has_value());
}

TEST(EditorRecovery_FailedRestoreLeavesCurrentWorldAndRecord)
{
    ::World current;
    const ::EntityID keep = current.CreateEntity("Keep Me");
    current.AddComponent<::Transform>(keep);

    SparkEditor::EditorRecoverySnapshot snapshot;
    snapshot.projectIdentity = "C:/Projects/project-a";
    snapshot.projectRelativeScene = "Scenes/Main.sparkscene";
    snapshot.sceneDisplayName = "Main";
    snapshot.serializedWorld = "malformed-world";

    SparkEditor::EditorRecoveryController controller;
    controller.Offer(snapshot);

    std::string error;
    const auto recovered = SparkEditor::DeserializeRecoverySnapshotIntoFreshWorld(snapshot, error);
    EXPECT_FALSE(recovered != nullptr);
    EXPECT_FALSE(error.empty());
    controller.SetRestoreFailure(error);
    ASSERT_TRUE(controller.Snapshot() != nullptr);
    EXPECT_EQ(static_cast<int>(controller.State()),
              static_cast<int>(SparkEditor::EditorRecoveryDialogState::RestoreFailed));
    EXPECT_TRUE(current.GetRegistry().valid(keep));
}

TEST(EditorRecovery_StrictRestoreRejectsPartialScenesWithoutTouchingLiveWorld)
{
    ::World current;
    const ::EntityID keep = current.CreateEntity("Keep Me");
    current.AddComponent<::Transform>(keep);

    SparkEditor::EditorRecoverySnapshot snapshot;
    snapshot.projectIdentity = "C:/Projects/project-a";
    snapshot.projectRelativeScene = "Scenes/Main.sparkscene";
    snapshot.sceneDisplayName = "Main";

    ::World validWorld;
    const ::EntityID restoredEntity = validWorld.CreateEntity("Recovered");
    validWorld.AddComponent<::Transform>(restoredEntity);
    snapshot = SparkEditor::CaptureRecoverySnapshotOnCallingThread(validWorld, std::move(snapshot));

    const std::string validWorldJson = snapshot.serializedWorld;
    const size_t positionMarker = validWorldJson.find("\"position\": \"");
    ASSERT_NE(positionMarker, std::string::npos);
    const size_t positionValueBegin = positionMarker + std::string("\"position\": \"").size();
    const size_t positionValueEnd = validWorldJson.find('"', positionValueBegin);
    ASSERT_NE(positionValueEnd, std::string::npos);

    std::string malformedField = validWorldJson;
    malformedField.replace(positionValueBegin, positionValueEnd - positionValueBegin, "not-a-vector");

    const std::array<std::string, 3> malformedWorlds = {
        R"json({"version":1,"entities":[{"id":1,"name":"Recovered","parent":-1,"components":{}}]})json",
        R"json({"version":1,"entities":[{"id":1,"name":"Recovered","parent":-1,"components":[{"type":"UnknownComponent","fields":{}}]}]})json",
        malformedField,
    };

    for (const std::string& malformed : malformedWorlds)
    {
        snapshot.serializedWorld = malformed;
        std::string error;
        const std::unique_ptr<::World> restored =
            SparkEditor::DeserializeRecoverySnapshotIntoFreshWorld(snapshot, error);
        EXPECT_FALSE(restored != nullptr);
        EXPECT_FALSE(error.empty());
        EXPECT_TRUE(current.GetRegistry().valid(keep));
    }
}

TEST(EditorRecovery_ExplicitDiscardClearsOnlyAfterUserAction)
{
    SparkEditor::EditorRecoverySnapshot snapshot;
    snapshot.projectIdentity = "project-a";
    snapshot.projectRelativeScene = "Scenes/Main.sparkscene";
    snapshot.sceneDisplayName = "Main";
    snapshot.serializedWorld = R"({"entities":[]})";

    SparkEditor::EditorRecoveryController controller;
    controller.Offer(snapshot);
    EXPECT_EQ(static_cast<int>(controller.State()),
              static_cast<int>(SparkEditor::EditorRecoveryDialogState::Available));
    ASSERT_TRUE(controller.Snapshot() != nullptr);

    controller.SetRestoreFailure("recovery document could not be deserialized");
    EXPECT_EQ(static_cast<int>(controller.State()),
              static_cast<int>(SparkEditor::EditorRecoveryDialogState::RestoreFailed));
    EXPECT_EQ(std::string(controller.Error()), std::string("recovery document could not be deserialized"));
    ASSERT_TRUE(controller.Snapshot() != nullptr);

    controller.DismissAfterRestore();
    EXPECT_EQ(static_cast<int>(controller.State()),
              static_cast<int>(SparkEditor::EditorRecoveryDialogState::Hidden));
    EXPECT_TRUE(controller.Snapshot() == nullptr);

    controller.Offer(snapshot);
    controller.DismissAfterDiscard();
    EXPECT_EQ(static_cast<int>(controller.State()),
              static_cast<int>(SparkEditor::EditorRecoveryDialogState::Hidden));
    EXPECT_TRUE(controller.Snapshot() == nullptr);
    EXPECT_TRUE(controller.Error().empty());
}

TEST(EditorRecovery_ExplicitDocumentDiscardSuppressesStaleCaptureUntilNewMutation)
{
    SparkEditor::EditorRecoveryCaptureGate gate;
    EXPECT_TRUE(gate.AllowsCapture());

    gate.SuppressAfterExplicitDiscard();
    EXPECT_FALSE(gate.AllowsCapture());

    gate.NoteNewMutation();
    EXPECT_TRUE(gate.AllowsCapture());

    gate.SuppressAfterExplicitDiscard();
    gate.ResetForNewDocument();
    EXPECT_TRUE(gate.AllowsCapture());
}

TEST(EditorRecovery_ClearForProjectPreservesForeignRecovery)
{
    ScopedRecoveryDirectory scratch;
    SparkEditor::EditorRecoveryStore store(scratch.Path());

    SparkEditor::EditorRecoverySnapshot projectA;
    projectA.projectIdentity = "project-a";
    projectA.projectRelativeScene = "Scenes/A.sparkscene";
    projectA.sceneDisplayName = "A";
    projectA.serializedWorld = R"({"entities":[]})";

    SparkEditor::EditorRecoverySnapshot projectB = projectA;
    projectB.projectIdentity = "project-b";
    projectB.projectRelativeScene = "Scenes/B.sparkscene";
    projectB.sceneDisplayName = "B";

    std::string error;
    ASSERT_TRUE(store.Save(projectA, error));
    ASSERT_TRUE(store.Save(projectB, error));

    const SparkEditor::EditorRecoveryLoadResult primaryForProjectA = store.LoadForProject(projectA.projectIdentity);
    EXPECT_EQ(static_cast<int>(primaryForProjectA.state), static_cast<int>(SparkEditor::EditorRecoveryLoadState::Primary));
    ASSERT_TRUE(primaryForProjectA.snapshot.has_value());
    EXPECT_EQ(primaryForProjectA.snapshot->sceneDisplayName, std::string("A"));

    ASSERT_TRUE(store.ClearForProject(projectA.projectIdentity, error));

    const SparkEditor::EditorRecoveryLoadResult projectALoad = store.LoadForProject(projectA.projectIdentity);
    EXPECT_EQ(static_cast<int>(projectALoad.state), static_cast<int>(SparkEditor::EditorRecoveryLoadState::None));

    const SparkEditor::EditorRecoveryLoadResult projectBLoad = store.LoadForProject(projectB.projectIdentity);
    EXPECT_EQ(static_cast<int>(projectBLoad.state), static_cast<int>(SparkEditor::EditorRecoveryLoadState::Primary));
    ASSERT_TRUE(projectBLoad.snapshot.has_value());
    EXPECT_EQ(projectBLoad.snapshot->sceneDisplayName, std::string("B"));
}

TEST(EditorRecovery_ClearForProjectRemovesReadableMalformedRecords)
{
    ScopedRecoveryDirectory scratch;
    SparkEditor::EditorRecoveryStore store(scratch.Path());

    SparkEditor::EditorRecoverySnapshot snapshot;
    snapshot.projectIdentity = "project-a";
    snapshot.projectRelativeScene = "Scenes/Main.sparkscene";
    snapshot.sceneDisplayName = "Main";
    snapshot.serializedWorld = R"({"entities":[]})";

    std::string error;
    ASSERT_TRUE(store.Save(snapshot, error));
    snapshot.dirtySequence = 1;
    ASSERT_TRUE(store.Save(snapshot, error));

    const fs::path primary = FindRecoveryFileForProject(scratch.Path(), snapshot.projectIdentity, "recovery-v1.json");
    const fs::path backup =
        FindRecoveryFileForProject(scratch.Path(), snapshot.projectIdentity, "recovery-v1.backup.json");
    ASSERT_FALSE(primary.empty());
    ASSERT_FALSE(backup.empty());
    ASSERT_TRUE(WriteAll(primary, "{ malformed recovery primary"));
    ASSERT_TRUE(WriteAll(backup, "{ malformed recovery backup"));

    const SparkEditor::EditorRecoveryLoadResult invalid = store.LoadForProject(snapshot.projectIdentity);
    EXPECT_EQ(static_cast<int>(invalid.state), static_cast<int>(SparkEditor::EditorRecoveryLoadState::Invalid));

    ASSERT_TRUE(store.ClearForProject(snapshot.projectIdentity, error));
    EXPECT_TRUE(FindRecoveryFileForProject(scratch.Path(), snapshot.projectIdentity, "recovery-v1.json").empty());
    EXPECT_TRUE(
        FindRecoveryFileForProject(scratch.Path(), snapshot.projectIdentity, "recovery-v1.backup.json").empty());

    const SparkEditor::EditorRecoveryLoadResult cleared = store.LoadForProject(snapshot.projectIdentity);
    EXPECT_EQ(static_cast<int>(cleared.state), static_cast<int>(SparkEditor::EditorRecoveryLoadState::None));
}

TEST(EditorRecovery_ProjectSnapshotsSurviveOtherProjectRecapture)
{
    ScopedRecoveryDirectory scratch;
    SparkEditor::EditorRecoveryStore store(scratch.Path());

    SparkEditor::EditorRecoverySnapshot projectA;
    projectA.projectIdentity = "project-a";
    projectA.projectRelativeScene = "Scenes/A.sparkscene";
    projectA.sceneDisplayName = "A";
    projectA.serializedWorld = R"({"entities":[]})";
    projectA.dirtySequence = 1;

    SparkEditor::EditorRecoverySnapshot projectB = projectA;
    projectB.projectIdentity = "project-b";
    projectB.projectRelativeScene = "Scenes/B.sparkscene";
    projectB.sceneDisplayName = "B";
    projectB.dirtySequence = 1;

    std::string error;
    ASSERT_TRUE(store.Save(projectA, error));
    ASSERT_TRUE(store.Save(projectB, error));
    projectB.dirtySequence = 2;
    ASSERT_TRUE(store.Save(projectB, error));

    const SparkEditor::EditorRecoveryLoadResult loadedA = store.LoadForProject(projectA.projectIdentity);
    EXPECT_EQ(static_cast<int>(loadedA.state), static_cast<int>(SparkEditor::EditorRecoveryLoadState::Primary));
    ASSERT_TRUE(loadedA.snapshot.has_value());
    EXPECT_EQ(loadedA.snapshot->dirtySequence, uint64_t(1));

    const SparkEditor::EditorRecoveryLoadResult loadedB = store.LoadForProject(projectB.projectIdentity);
    EXPECT_EQ(static_cast<int>(loadedB.state), static_cast<int>(SparkEditor::EditorRecoveryLoadState::Primary));
    ASSERT_TRUE(loadedB.snapshot.has_value());
    EXPECT_EQ(loadedB.snapshot->dirtySequence, uint64_t(2));
}

TEST(EditorRecovery_ProjectPathResolutionRejectsEscapes)
{
    ScopedRecoveryDirectory scratch;
    const fs::path projectRoot = scratch.Path() / "Project";
    fs::create_directories(projectRoot / "Scenes");

    fs::path resolved;
    std::string error;
    EXPECT_TRUE(SparkEditor::ResolvePathInsideProject(projectRoot, fs::path("Scenes/Main.sparkscene"), resolved,
                                                       error));
    EXPECT_FALSE(resolved.empty());
    EXPECT_FALSE(SparkEditor::ResolvePathInsideProject(projectRoot, fs::path("../outside.sparkscene"), resolved,
                                                        error));

    const fs::path outside = scratch.Path() / "Outside";
    fs::create_directories(outside);
    EXPECT_FALSE(SparkEditor::ResolvePathInsideProject(projectRoot, outside / "outside.sparkscene", resolved,
                                                        error));
    std::error_code symlinkError;
    fs::create_directory_symlink(outside, projectRoot / "Scenes" / "linked", symlinkError);
    if (!symlinkError)
    {
        EXPECT_FALSE(SparkEditor::ResolvePathInsideProject(
            projectRoot, fs::path("Scenes/linked/escaped.sparkscene"), resolved, error));
    }
}
