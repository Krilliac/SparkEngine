/**
 * @file EditorRecovery.cpp
 * @brief Versioned JSON persistence for editor recovery snapshots.
 */

#include "EditorRecovery.h"

#include "Utils/JsonUtils.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
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
            if (path.is_absolute() || path.has_root_path())
                return false;

            for (const auto& segment : path)
            {
                if (segment == "..")
                    return false;
            }
            return true;
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
            if (snapshot.sceneDisplayName.empty())
            {
                error = "recovery scene display name is empty";
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
            root["capturedUnixMilliseconds"] = Spark::Json::Value(static_cast<double>(snapshot.capturedUnixMilliseconds));
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
                dirtySequence > static_cast<double>(kMaxExactJsonInteger) || std::trunc(dirtySequence) != dirtySequence ||
                !std::isfinite(capturedTime) ||
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

    bool EditorRecoveryStore::Save(const EditorRecoverySnapshot& snapshot, std::string& error)
    {
        error.clear();
        if (!ValidateSnapshot(snapshot, error))
            return false;

        std::error_code filesystemError;
        if (m_directory.empty() || (!fs::create_directories(m_directory, filesystemError) && filesystemError))
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

        const fs::path primary = m_directory / "recovery-v1.json";
        const fs::path backup = m_directory / "recovery-v1.backup.json";
        const fs::path temporary = TemporarySibling(primary);
        if (!WriteAndVerify(temporary, document, error))
        {
            std::error_code cleanupError;
            fs::remove(temporary, cleanupError);
            return false;
        }

        if (!RotatePrimaryToBackup(primary, backup, error))
        {
            std::error_code cleanupError;
            fs::remove(temporary, cleanupError);
            return false;
        }

        if (!ReplaceAtomically(temporary, primary, error))
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
        bool cleared = true;
        for (const fs::path& path : {m_directory / "recovery-v1.json", m_directory / "recovery-v1.backup.json"})
        {
            std::error_code filesystemError;
            fs::remove(path, filesystemError);
            if (filesystemError)
            {
                if (!error.empty())
                    error += "; ";
                error += "cannot remove recovery file " + path.filename().string() + ": " + filesystemError.message();
                cleared = false;
            }
        }
        return cleared;
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
        const fs::path primary = m_directory / "recovery-v1.json";
        const ParsedRecoveryFile primaryFile = ReadRecoveryFile(primary);
        if (primaryFile.snapshot)
        {
            if (primaryFile.snapshot->projectIdentity != projectIdentity)
                return result;
            result.state = EditorRecoveryLoadState::Primary;
            result.snapshot = primaryFile.snapshot;
            return result;
        }

        const fs::path backup = m_directory / "recovery-v1.backup.json";
        const ParsedRecoveryFile backupFile = ReadRecoveryFile(backup);
        if (backupFile.snapshot && backupFile.snapshot->projectIdentity == projectIdentity)
        {
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
