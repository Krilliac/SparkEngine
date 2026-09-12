/**
 * @file EditorRecovery.cpp
 * @brief Versioned JSON persistence for editor recovery snapshots.
 */

#include "EditorRecovery.h"

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
    namespace
    {
        namespace fs = std::filesystem;

        constexpr size_t kMaxOperationCount = 50;
        constexpr size_t kMaxOperationBytes = 4096;
        constexpr size_t kMaxProjectIdentityBytes = 4096;
        constexpr size_t kMaxSceneDisplayNameBytes = 1024;
        constexpr size_t kMaxProjectRelativePathBytes = 4096;
        constexpr uint64_t kMaxExactJsonInteger = 9007199254740991ULL;

        Spark::Json::JsonLimits RecoveryJsonLimits()
        {
            Spark::Json::JsonLimits limits;
            limits.maxBytes = kEditorRecoveryMaxBytes;
            limits.maxDepth = 64;
            limits.maxNodes = 250000;
            return limits;
        }

        bool ParseRecoveryJson(std::string_view text, Spark::Json::Value& value, std::string& error)
        {
            return Spark::Json::ParseBounded(text, RecoveryJsonLimits(), &value, &error);
        }

        bool IsSafeRelativePath(const std::string& value)
        {
            if (value.empty())
                return true;

            const fs::path path(value);
            if (path.is_absolute() || path.has_root_path() || path.has_root_name())
                return false;

            for (const auto& segment : path)
            {
                if (segment == "..")
                    return false;
            }
            return true;
        }

        std::string EncodeProjectIdentityForPath(std::string_view projectIdentity)
        {
            constexpr char hex[] = "0123456789abcdef";
            std::string encoded;
            encoded.reserve(projectIdentity.size() * 2u);
            for (const unsigned char byte : projectIdentity)
            {
                encoded.push_back(hex[byte >> 4u]);
                encoded.push_back(hex[byte & 0x0fu]);
            }
            return encoded;
        }

        struct RecoveryFiles
        {
            fs::path primary;
            fs::path backup;
        };

        RecoveryFiles RecoveryFilesForProject(const fs::path& root, std::string_view projectIdentity)
        {
            // Split the reversible hex key into legal path components. This is
            // collision-free (unlike a short filename hash) and does not put
            // the raw project path into a filesystem component.
            constexpr size_t kKeySegmentLength = 96;
            const std::string key = EncodeProjectIdentityForPath(projectIdentity);
            fs::path directory = root / "recovery-v1";
            for (size_t offset = 0; offset < key.size(); offset += kKeySegmentLength)
                directory /= key.substr(offset, kKeySegmentLength);
            return {directory / "recovery-v1.json", directory / "recovery-v1.backup.json"};
        }

        bool ValidateSnapshot(const EditorRecoverySnapshot& snapshot, std::string& error)
        {
            if (snapshot.schemaVersion != kEditorRecoverySchemaVersion)
            {
                error = "unsupported recovery schema version";
                return false;
            }
            if (snapshot.projectIdentity.empty())
            {
                error = "recovery project identity is empty";
                return false;
            }
            if (snapshot.projectIdentity.size() > kMaxProjectIdentityBytes)
            {
                error = "recovery project identity exceeds the size limit";
                return false;
            }
            if (snapshot.sceneDisplayName.empty())
            {
                error = "recovery scene display name is empty";
                return false;
            }
            if (snapshot.sceneDisplayName.size() > kMaxSceneDisplayNameBytes)
            {
                error = "recovery scene display name exceeds the size limit";
                return false;
            }
            if (snapshot.projectRelativeScene.size() > kMaxProjectRelativePathBytes ||
                snapshot.layoutIniPath.size() > kMaxProjectRelativePathBytes)
            {
                error = "recovery relative path exceeds the size limit";
                return false;
            }
            if (!IsSafeRelativePath(snapshot.projectRelativeScene))
            {
                error = "recovery scene path is not project-relative";
                return false;
            }
            if (!IsSafeRelativePath(snapshot.layoutIniPath))
            {
                error = "recovery layout path is not project-relative";
                return false;
            }
            if (snapshot.serializedWorld.empty())
            {
                error = "recovery world is empty";
                return false;
            }
            if (snapshot.serializedWorld.size() > kEditorRecoveryMaxBytes)
            {
                error = "recovery world exceeds the size limit";
                return false;
            }
            if (snapshot.dirtySequence > kMaxExactJsonInteger ||
                snapshot.capturedUnixMilliseconds > static_cast<int64_t>(kMaxExactJsonInteger) ||
                snapshot.capturedUnixMilliseconds < -static_cast<int64_t>(kMaxExactJsonInteger))
            {
                error = "recovery sequence or timestamp exceeds JSON integer precision";
                return false;
            }
            if (snapshot.recentOperations.size() > kMaxOperationCount)
            {
                error = "recovery operation count exceeds the limit";
                return false;
            }
            for (const std::string& operation : snapshot.recentOperations)
            {
                if (operation.size() > kMaxOperationBytes)
                {
                    error = "recovery operation exceeds the size limit";
                    return false;
                }
            }

            Spark::Json::Value world;
            if (!ParseRecoveryJson(snapshot.serializedWorld, world, error))
            {
                error = "recovery world is not valid JSON: " + error;
                return false;
            }
            if (!world.IsObject())
            {
                error = "recovery world root is not an object";
                return false;
            }
            return true;
        }

        Spark::Json::Value SnapshotToJson(const EditorRecoverySnapshot& snapshot)
        {
            Spark::Json::Value root = Spark::Json::Value::MakeObject();
            root["schemaVersion"] = Spark::Json::Value(static_cast<int>(snapshot.schemaVersion));
            root["projectIdentity"] = Spark::Json::Value(snapshot.projectIdentity);
            root["projectRelativeScene"] = Spark::Json::Value(snapshot.projectRelativeScene);
            root["sceneDisplayName"] = Spark::Json::Value(snapshot.sceneDisplayName);
            root["serializedWorld"] = Spark::Json::Value(snapshot.serializedWorld);
            root["layoutIniPath"] = Spark::Json::Value(snapshot.layoutIniPath);
            root["dirtySequence"] = Spark::Json::Value(static_cast<double>(snapshot.dirtySequence));
            root["capturedUnixMilliseconds"] =
                Spark::Json::Value(static_cast<double>(snapshot.capturedUnixMilliseconds));
            root["recentOperations"] = Spark::Json::Value::MakeArray();
            for (const std::string& operation : snapshot.recentOperations)
                root["recentOperations"].PushBack(Spark::Json::Value(operation));
            return root;
        }

        bool ReadStringField(const Spark::Json::Value& root, const char* key, std::string& output, std::string& error)
        {
            const std::string name(key);
            if (!root.HasKey(name) || !root[name].IsString())
            {
                error = "recovery field is missing or not a string: " + name;
                return false;
            }
            output = root[name].AsString();
            return true;
        }

        std::optional<EditorRecoverySnapshot> SnapshotFromJson(const Spark::Json::Value& root, std::string& error)
        {
            if (!root.IsObject())
            {
                error = "recovery root is not an object";
                return std::nullopt;
            }

            if (!root.HasKey("schemaVersion") || !root["schemaVersion"].IsNumber())
            {
                error = "recovery schema version is missing";
                return std::nullopt;
            }

            const double schemaNumber = root["schemaVersion"].AsNumber();
            if (!std::isfinite(schemaNumber) || schemaNumber != static_cast<double>(kEditorRecoverySchemaVersion))
            {
                error = "unsupported recovery schema version";
                return std::nullopt;
            }

            EditorRecoverySnapshot snapshot;
            if (!ReadStringField(root, "projectIdentity", snapshot.projectIdentity, error) ||
                !ReadStringField(root, "projectRelativeScene", snapshot.projectRelativeScene, error) ||
                !ReadStringField(root, "sceneDisplayName", snapshot.sceneDisplayName, error) ||
                !ReadStringField(root, "serializedWorld", snapshot.serializedWorld, error) ||
                !ReadStringField(root, "layoutIniPath", snapshot.layoutIniPath, error))
            {
                return std::nullopt;
            }

            if (!root.HasKey("dirtySequence") || !root["dirtySequence"].IsNumber() ||
                !root.HasKey("capturedUnixMilliseconds") || !root["capturedUnixMilliseconds"].IsNumber())
            {
                error = "recovery sequence or timestamp is missing";
                return std::nullopt;
            }

            const double dirtySequence = root["dirtySequence"].AsNumber();
            const double capturedTime = root["capturedUnixMilliseconds"].AsNumber();
            if (!std::isfinite(dirtySequence) || dirtySequence < 0.0 ||
                dirtySequence > static_cast<double>(kMaxExactJsonInteger) ||
                std::trunc(dirtySequence) != dirtySequence || !std::isfinite(capturedTime) ||
                capturedTime < -static_cast<double>(kMaxExactJsonInteger) ||
                capturedTime > static_cast<double>(kMaxExactJsonInteger) || std::trunc(capturedTime) != capturedTime)
            {
                error = "recovery sequence or timestamp is outside its supported range";
                return std::nullopt;
            }
            snapshot.dirtySequence = static_cast<uint64_t>(dirtySequence);
            snapshot.capturedUnixMilliseconds = static_cast<int64_t>(capturedTime);

            if (!root.HasKey("recentOperations") || !root["recentOperations"].IsArray())
            {
                error = "recovery operations are missing";
                return std::nullopt;
            }
            if (root["recentOperations"].Size() > kMaxOperationCount)
            {
                error = "recovery operation count exceeds the limit";
                return std::nullopt;
            }
            for (size_t index = 0; index < root["recentOperations"].Size(); ++index)
            {
                const Spark::Json::Value& value = root["recentOperations"][index];
                if (!value.IsString() || value.AsString().size() > kMaxOperationBytes)
                {
                    error = "recovery operation is malformed";
                    return std::nullopt;
                }
                snapshot.recentOperations.push_back(value.AsString());
            }

            if (!ValidateSnapshot(snapshot, error))
                return std::nullopt;
            return snapshot;
        }

        bool ReadFile(const fs::path& path, std::string& bytes, std::string& error)
        {
            std::error_code filesystemError;
            const uintmax_t size = fs::file_size(path, filesystemError);
            if (filesystemError || size > kEditorRecoveryMaxBytes)
            {
                error = filesystemError ? "cannot read recovery file: " + filesystemError.message()
                                        : "recovery file exceeds the size limit";
                return false;
            }

            std::ifstream input(path, std::ios::binary);
            if (!input)
            {
                error = "cannot open recovery file";
                return false;
            }
            bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
            if (!input.good() && !input.eof())
            {
                error = "cannot read recovery file";
                return false;
            }
            return true;
        }

        fs::path TemporarySibling(const fs::path& destination)
        {
            static std::atomic<uint64_t> counter{0};
            const uint64_t nonce = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                                   counter.fetch_add(1, std::memory_order_relaxed);
            return destination.parent_path() / (destination.filename().string() + ".tmp." + std::to_string(nonce));
        }

        bool ReplaceAtomically(const fs::path& temporary, const fs::path& destination, std::string& error)
        {
#ifdef _WIN32
            if (::MoveFileExW(temporary.c_str(), destination.c_str(),
                              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                return true;
            }
            error = "cannot atomically replace recovery file: " +
                    std::error_code(static_cast<int>(::GetLastError()), std::system_category()).message();
            return false;
#else
            std::error_code filesystemError;
            fs::rename(temporary, destination, filesystemError);
            if (!filesystemError)
                return true;
            error = "cannot atomically replace recovery file: " + filesystemError.message();
            return false;
#endif
        }

        bool WriteAndVerify(const fs::path& path, std::string_view document, std::string& error)
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                error = "cannot open temporary recovery file";
                return false;
            }
            output.write(document.data(), static_cast<std::streamsize>(document.size()));
            output.close();
            if (!output.good())
            {
                error = "cannot write temporary recovery file";
                return false;
            }

            std::string reread;
            if (!ReadFile(path, reread, error) || reread != document)
            {
                if (error.empty())
                    error = "temporary recovery file did not round-trip";
                return false;
            }

            Spark::Json::Value root;
            return ParseRecoveryJson(reread, root, error);
        }

        struct ParsedRecoveryFile
        {
            bool exists = false;
            bool readable = false;
            std::optional<EditorRecoverySnapshot> snapshot;
            std::string document;
            std::string error;
        };

        ParsedRecoveryFile ReadRecoveryFile(const fs::path& path)
        {
            ParsedRecoveryFile result;
            std::error_code filesystemError;
            result.exists = fs::exists(path, filesystemError);
            if (filesystemError)
            {
                result.error = "cannot inspect recovery file: " + filesystemError.message();
                return result;
            }
            if (!result.exists)
                return result;

            if (!ReadFile(path, result.document, result.error))
                return result;
            result.readable = true;

            Spark::Json::Value root;
            if (!ParseRecoveryJson(result.document, root, result.error))
                return result;

            result.snapshot = SnapshotFromJson(root, result.error);
            return result;
        }

        bool RotatePrimaryToBackup(const fs::path& primary, const fs::path& backup, std::string& error)
        {
            ParsedRecoveryFile existing = ReadRecoveryFile(primary);
            if (!existing.exists)
            {
                if (!existing.error.empty())
                {
                    error = existing.error;
                    return false;
                }
                return true;
            }

            if (!existing.readable)
            {
                error = "cannot preserve existing recovery snapshot: " + existing.error;
                return false;
            }

            // A malformed primary cannot be a useful fallback; leave the last known-good
            // backup untouched while replacing it with the newly validated snapshot.
            if (!existing.snapshot)
                return true;

            const fs::path temporary = TemporarySibling(backup);
            if (!WriteAndVerify(temporary, existing.document, error))
            {
                std::error_code cleanupError;
                fs::remove(temporary, cleanupError);
                return false;
            }
            if (!ReplaceAtomically(temporary, backup, error))
            {
                std::error_code cleanupError;
                fs::remove(temporary, cleanupError);
                return false;
            }
            return true;
        }
    } // namespace

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
