/**
 * @file EditorRecovery.cpp
 * @brief Versioned JSON persistence for editor recovery snapshots.
 */

#include "EditorRecovery.h"

#include "EditorRecoveryFiles.h"

#include "Engine/ECS/Components.h"
#include "SceneManager/ReflectedSceneSerializer.h"
#include "Utils/JsonUtils.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <memory>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace SparkEditor
{
    using namespace RecoveryDetail;


    EditorRecoveryStore::EditorRecoveryStore(fs::path directory) : m_directory(std::move(directory)) {}

    EditorRecoverySnapshot CaptureRecoverySnapshotOnCallingThread(const ::World& world, EditorRecoverySnapshot metadata)
    {
        metadata.serializedWorld = Spark::SerializeWorld(world);
        return metadata;
    }

    bool ResolvePathInsideProject(const fs::path& projectRoot, const fs::path& candidate, fs::path& resolved,
                                  std::string& error)
    {
        error.clear();
        resolved.clear();
        if (projectRoot.empty() || candidate.empty())
        {
            error = "project root or candidate path is empty";
            return false;
        }

        std::error_code filesystemError;
        const fs::path canonicalRoot = fs::weakly_canonical(projectRoot, filesystemError);
        if (filesystemError || canonicalRoot.empty())
        {
            error = "project root could not be canonicalized";
            return false;
        }

        const fs::path requested = candidate.is_absolute() ? candidate : canonicalRoot / candidate;
        const fs::path canonicalCandidate = fs::weakly_canonical(requested, filesystemError);
        if (filesystemError || canonicalCandidate.empty())
        {
            error = "candidate path could not be canonicalized";
            return false;
        }

        const fs::path relative = canonicalCandidate.lexically_relative(canonicalRoot);
        if (relative.empty() || relative == "." || relative.is_absolute() || relative.has_root_path() ||
            relative.has_root_name() || *relative.begin() == "..")
        {
            error = "candidate path escapes the active project";
            return false;
        }

        resolved = canonicalCandidate;
        return true;
    }

    std::unique_ptr<::World> DeserializeRecoverySnapshotIntoFreshWorld(const EditorRecoverySnapshot& snapshot,
                                                                       std::string& error)
    {
        error.clear();
        // This helper is deliberately safe even if a future caller bypasses
        // EditorRecoveryStore. Validate the bounded envelope before handing
        // its nested JSON to the scene deserializer.
        if (!ValidateSnapshot(snapshot, error))
        {
            error = "recovery document is invalid: " + error;
            return {};
        }

        try
        {
            auto restored = std::make_unique<::World>();
            if (!Spark::DeserializeInto(*restored, snapshot.serializedWorld,
                                        Spark::SceneDeserializeMode::StrictRecovery))
            {
                error = "recovery document could not be deserialized";
                return {};
            }
            return restored;
        }
        catch (const std::exception& exception)
        {
            error = "recovery document could not be deserialized: " + std::string(exception.what());
            return {};
        }
    }

    void EditorRecoveryController::Offer(EditorRecoverySnapshot snapshot, bool usedBackup)
    {
        m_snapshot = std::move(snapshot);
        m_error.clear();
        m_usesBackup = usedBackup;
        m_state = EditorRecoveryDialogState::Available;
    }

    void EditorRecoveryController::SetRestoreFailure(std::string error)
    {
        if (!m_snapshot)
            return;
        m_error = error.empty() ? "recovery document could not be restored" : std::move(error);
        m_state = EditorRecoveryDialogState::RestoreFailed;
    }

    void EditorRecoveryController::DismissAfterRestore()
    {
        m_snapshot.reset();
        m_error.clear();
        m_usesBackup = false;
        m_state = EditorRecoveryDialogState::Hidden;
    }

    void EditorRecoveryController::DismissAfterDiscard()
    {
        m_snapshot.reset();
        m_error.clear();
        m_usesBackup = false;
        m_state = EditorRecoveryDialogState::Hidden;
    }

    const EditorRecoverySnapshot* EditorRecoveryController::Snapshot() const
    {
        return m_snapshot ? &*m_snapshot : nullptr;
    }

    bool EditorRecoveryStore::Save(const EditorRecoverySnapshot& snapshot, std::string& error)
    {
        error.clear();
        if (!ValidateSnapshot(snapshot, error))
            return false;

        std::error_code filesystemError;
        if (m_directory.empty())
        {
            error = "recovery directory is empty";
            return false;
        }

        const RecoveryFiles files = RecoveryFilesForProject(m_directory, snapshot.projectIdentity);
        if (!fs::create_directories(files.primary.parent_path(), filesystemError) && filesystemError)
        {
            error = "cannot create recovery directory";
            if (filesystemError)
                error += ": " + filesystemError.message();
            return false;
        }

        const std::string document = Spark::Json::StringifyPretty(SnapshotToJson(snapshot));
        Spark::Json::Value validated;
        if (!ParseRecoveryJson(document, validated, error))
            return false;

        const fs::path temporary = TemporarySibling(files.primary);
        if (!WriteAndVerify(temporary, document, error))
        {
            std::error_code cleanupError;
            fs::remove(temporary, cleanupError);
            return false;
        }

        if (!RotatePrimaryToBackup(files.primary, files.backup, error))
        {
            std::error_code cleanupError;
            fs::remove(temporary, cleanupError);
            return false;
        }

        if (!ReplaceAtomically(temporary, files.primary, error))
        {
            std::error_code cleanupError;
            fs::remove(temporary, cleanupError);
            return false;
        }
        return true;
    }

    bool EditorRecoveryStore::Clear(std::string& error) const
    {
        error.clear();
        if (m_directory.empty())
        {
            error = "recovery directory is empty";
            return false;
        }
        std::error_code filesystemError;
        fs::remove_all(m_directory / "recovery-v1", filesystemError);
        if (filesystemError)
        {
            error = "cannot remove recovery namespace: " + filesystemError.message();
            return false;
        }
        return true;
    }

    bool EditorRecoveryStore::ClearForProject(std::string_view projectIdentity, std::string& error) const
    {
        error.clear();
        if (m_directory.empty())
        {
            error = "recovery directory is empty";
            return false;
        }
        if (projectIdentity.empty())
        {
            error = "recovery project identity is empty";
            return false;
        }
        if (projectIdentity.size() > kMaxProjectIdentityBytes)
        {
            error = "recovery project identity exceeds the size limit";
            return false;
        }

        const RecoveryFiles files = RecoveryFilesForProject(m_directory, projectIdentity);
        const std::array<fs::path, 2> paths = {files.primary, files.backup};
        std::vector<fs::path> matchingPaths;
        for (const fs::path& path : paths)
        {
            const ParsedRecoveryFile file = ReadRecoveryFile(path);
            if (file.exists && !file.readable)
            {
                error = "cannot inspect recovery file " + path.filename().string() + ": " + file.error;
                return false;
            }
            if (!file.exists)
                continue;
            if (!file.snapshot)
            {
                // This is an expected, deterministic path inside this project's
                // namespace. A readable malformed record cannot be restored and
                // must not survive an explicit discard or a successful save.
                matchingPaths.push_back(path);
                continue;
            }
            if (file.snapshot)
            {
                if (file.snapshot->projectIdentity != projectIdentity)
                {
                    error = "recovery record project identity does not match its storage namespace";
                    return false;
                }
                matchingPaths.push_back(path);
            }
        }

        for (const fs::path& path : matchingPaths)
        {
            std::error_code filesystemError;
            fs::remove(path, filesystemError);
            if (filesystemError)
            {
                error = "cannot remove recovery file " + path.filename().string() + ": " + filesystemError.message();
                return false;
            }
        }
        return true;
    }

    EditorRecoveryLoadResult EditorRecoveryStore::LoadForProject(std::string_view projectIdentity) const
    {
        EditorRecoveryLoadResult result;
        if (m_directory.empty())
        {
            result.state = EditorRecoveryLoadState::Invalid;
            result.error = "recovery directory is empty";
            return result;
        }
        if (projectIdentity.empty())
        {
            result.state = EditorRecoveryLoadState::Invalid;
            result.error = "recovery project identity is empty";
            return result;
        }
        if (projectIdentity.size() > kMaxProjectIdentityBytes)
        {
            result.state = EditorRecoveryLoadState::Invalid;
            result.error = "recovery project identity exceeds the size limit";
            return result;
        }

        const RecoveryFiles files = RecoveryFilesForProject(m_directory, projectIdentity);
        const ParsedRecoveryFile primaryFile = ReadRecoveryFile(files.primary);
        if (primaryFile.snapshot)
        {
            if (primaryFile.snapshot->projectIdentity != projectIdentity)
            {
                result.state = EditorRecoveryLoadState::Invalid;
                result.error = "primary recovery project identity does not match its storage namespace";
                return result;
            }
            result.state = EditorRecoveryLoadState::Primary;
            result.snapshot = primaryFile.snapshot;
            return result;
        }

        const ParsedRecoveryFile backupFile = ReadRecoveryFile(files.backup);
        if (backupFile.snapshot)
        {
            if (backupFile.snapshot->projectIdentity != projectIdentity)
            {
                result.state = EditorRecoveryLoadState::Invalid;
                result.error = "backup recovery project identity does not match its storage namespace";
                return result;
            }
            result.state = EditorRecoveryLoadState::Backup;
            result.snapshot = backupFile.snapshot;
            return result;
        }

        if (!primaryFile.error.empty() || !backupFile.error.empty())
        {
            result.state = EditorRecoveryLoadState::Invalid;
            if (!primaryFile.error.empty())
                result.error = "primary recovery: " + primaryFile.error;
            if (!backupFile.error.empty())
            {
                if (!result.error.empty())
                    result.error += "; ";
                result.error += "backup recovery: " + backupFile.error;
            }
            return result;
        }

        return result;
    }
} // namespace SparkEditor
