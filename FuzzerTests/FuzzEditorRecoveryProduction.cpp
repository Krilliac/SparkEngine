/**
 * @file FuzzEditorRecoveryProduction.cpp
 * @brief libc++-compiled production adapter for the editor recovery snapshot libFuzzer harness.
 *
 * At startup EditorUI asks EditorRecoveryStore::LoadForProject for the open project's
 * recovery snapshot: the primary file, else the backup, each read through one bounded
 * handle, parsed as a bounded JSON envelope and validated down to a strict deserialization
 * of the nested world. The fuzz bytes up to the first NUL become the project's primary
 * recovery file; the bytes after it, when there is a NUL, become its backup. A violated
 * contract aborts so libFuzzer records a crash:
 *  - the store offers the primary snapshot when that file holds one for this project, else
 *    the backup's, reports Invalid when either file was unusable or holds another project's
 *    snapshot, and None only when neither file exists;
 *  - an offered snapshot names this project, passes ValidateSnapshot again and restores into
 *    a fresh World through DeserializeRecoverySnapshotIntoFreshWorld;
 *  - an offered snapshot saved through EditorRecoveryStore::Save into another store loads
 *    back as the identical snapshot.
 *
 * World's SPARK_REQUIRE checks end in Assert::Fail; as SparkFuzzReflectedScene does, the
 * adapter defines that fatal sink as print-and-abort.
 */

#include "FuzzEditorRecoveryProduction.h"

#include "Core/EditorRecovery.h"
#include "Core/EditorRecoveryFiles.h"
#include "Engine/ECS/Components.h"
#include "Utils/Assert.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;
    constexpr std::string_view kIdentity = "/projects/fuzz/Fuzz.sparkproject";

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzEditorRecovery: EditorRecoveryStore::LoadForProject violated: %s\n", what);
        std::abort();
    }

    struct Fixture
    {
        std::filesystem::path load;
        std::filesystem::path save;
    };

    const Fixture& FixtureDirectories()
    {
        static const Fixture fixture = []
        {
            std::string pattern =
                (std::filesystem::temp_directory_path() / "spark-fuzz-editor-recovery-XXXXXX").string();
            if (::mkdtemp(pattern.data()) == nullptr)
            {
                InvariantFailure("could not create the fixture directory");
            }
            const std::filesystem::path base(pattern);
            return Fixture{base / "load", base / "save"};
        }();
        return fixture;
    }

    void WriteFile(const std::filesystem::path& path, std::string_view bytes)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out)
        {
            InvariantFailure("could not write a fixture file");
        }
    }

    bool SameSnapshot(const SparkEditor::EditorRecoverySnapshot& a, const SparkEditor::EditorRecoverySnapshot& b)
    {
        return a.schemaVersion == b.schemaVersion && a.projectIdentity == b.projectIdentity &&
               a.projectRelativeScene == b.projectRelativeScene && a.sceneDisplayName == b.sceneDisplayName &&
               a.serializedWorld == b.serializedWorld && a.layoutIniPath == b.layoutIniPath &&
               a.recentOperations == b.recentOperations && a.dirtySequence == b.dirtySequence &&
               a.capturedUnixMilliseconds == b.capturedUnixMilliseconds;
    }

    /// The state LoadForProject must report, from the per-file reader it uses.
    SparkEditor::EditorRecoveryLoadState ExpectedState(const SparkEditor::RecoveryDetail::RecoveryFiles& files)
    {
        using SparkEditor::EditorRecoveryLoadState;
        const auto primary = SparkEditor::RecoveryDetail::ReadRecoveryFile(files.primary);
        if (primary.snapshot)
        {
            return primary.snapshot->projectIdentity == kIdentity ? EditorRecoveryLoadState::Primary
                                                                  : EditorRecoveryLoadState::Invalid;
        }
        const auto backup = SparkEditor::RecoveryDetail::ReadRecoveryFile(files.backup);
        if (backup.snapshot)
        {
            return backup.snapshot->projectIdentity == kIdentity ? EditorRecoveryLoadState::Backup
                                                                 : EditorRecoveryLoadState::Invalid;
        }
        if (!primary.error.empty() || !backup.error.empty())
        {
            return EditorRecoveryLoadState::Invalid;
        }
        return EditorRecoveryLoadState::None;
    }
} // namespace

// Fatal sink for World's SPARK_REQUIRE/ASSERT_ALWAYS checks (see FuzzReflectedSceneProduction.cpp).
void Assert::Fail(const char* expr, const char* file, int line, const char* /*fmt*/, ...)
{
    std::fprintf(stderr, "SparkFuzzEditorRecovery: precondition '%s' failed at %s:%d\n", expr ? expr : "?",
                 file ? file : "?", line);
    std::abort();
}

extern "C" int SparkFuzzLoadEditorRecovery(const std::uint8_t* data, std::size_t size, std::uint32_t maxDepth)
{
    if (maxDepth != SparkEditor::RecoveryDetail::RecoveryJsonLimits().maxDepth)
    {
        InvariantFailure("the harness depth budget no longer matches RecoveryJsonLimits().maxDepth");
    }
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    const std::string_view input =
        size == 0 ? std::string_view() : std::string_view(reinterpret_cast<const char*>(data), size);
    const Fixture& fixture = FixtureDirectories();

    const auto files = SparkEditor::RecoveryDetail::RecoveryFilesForProject(fixture.load, kIdentity);
    std::error_code ignored;
    std::filesystem::create_directories(files.primary.parent_path(), ignored);
    const std::size_t split = input.find('\0');
    WriteFile(files.primary, input.substr(0, split));
    std::filesystem::remove(files.backup, ignored);
    if (split != std::string_view::npos)
    {
        WriteFile(files.backup, input.substr(split + 1));
    }

    const SparkEditor::EditorRecoveryStore store(fixture.load);
    const SparkEditor::EditorRecoveryLoadResult result = store.LoadForProject(kIdentity);
    if (result.state != ExpectedState(files))
    {
        InvariantFailure("the store chose a different file or state than the files hold");
    }
    const bool offered = result.state == SparkEditor::EditorRecoveryLoadState::Primary ||
                         result.state == SparkEditor::EditorRecoveryLoadState::Backup;
    if (result.snapshot.has_value() != offered)
    {
        InvariantFailure("a snapshot is present exactly when the state is Primary or Backup");
    }
    if (!offered)
    {
        return 0;
    }

    const SparkEditor::EditorRecoverySnapshot& snapshot = *result.snapshot;
    if (snapshot.projectIdentity != kIdentity)
    {
        InvariantFailure("the offered snapshot names another project");
    }
    std::string error;
    if (!SparkEditor::RecoveryDetail::ValidateSnapshot(snapshot, error))
    {
        InvariantFailure("the offered snapshot fails ValidateSnapshot");
    }
    if (!SparkEditor::DeserializeRecoverySnapshotIntoFreshWorld(snapshot, error))
    {
        InvariantFailure("the offered snapshot cannot be restored into a fresh World");
    }

    std::filesystem::remove_all(fixture.save, ignored);
    SparkEditor::EditorRecoveryStore saveStore(fixture.save);
    if (!saveStore.Save(snapshot, error))
    {
        InvariantFailure("the offered snapshot cannot be saved again");
    }
    const SparkEditor::EditorRecoveryLoadResult again = saveStore.LoadForProject(kIdentity);
    if (again.state != SparkEditor::EditorRecoveryLoadState::Primary || !again.snapshot ||
        !SameSnapshot(*again.snapshot, snapshot))
    {
        InvariantFailure("save -> load changed the snapshot");
    }
    return 0;
}
