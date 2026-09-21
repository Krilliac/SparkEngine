/**
 * @file EditorRecoveryFiles.h
 * @brief Internal filesystem helpers for editor recovery persistence.
 */

#pragma once

#include "EditorRecoveryJson.h"

#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace SparkEditor::RecoveryDetail
{
    namespace fs = std::filesystem;

    inline std::string EncodeProjectIdentityForPath(std::string_view projectIdentity)
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

    inline RecoveryFiles RecoveryFilesForProject(const fs::path& root, std::string_view projectIdentity)
    {
        constexpr size_t kKeySegmentLength = 96;
        const std::string key = EncodeProjectIdentityForPath(projectIdentity);
        fs::path directory = root / "recovery-v1";
        for (size_t offset = 0; offset < key.size(); offset += kKeySegmentLength)
            directory /= key.substr(offset, kKeySegmentLength);
        return {directory / "recovery-v1.json", directory / "recovery-v1.backup.json"};
    }

    inline bool ReadFile(const fs::path& path, std::string& bytes, std::string& error)
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

    inline fs::path TemporarySibling(const fs::path& destination)
    {
        static std::atomic<uint64_t> counter{0};
        const uint64_t nonce = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                               counter.fetch_add(1, std::memory_order_relaxed);
        return destination.parent_path() / (destination.filename().string() + ".tmp." + std::to_string(nonce));
    }

    inline bool ReplaceAtomically(const fs::path& temporary, const fs::path& destination, std::string& error)
    {
#ifdef _WIN32
        if (::MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
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

    inline bool WriteAndVerify(const fs::path& path, std::string_view document, std::string& error)
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

    inline ParsedRecoveryFile ReadRecoveryFile(const fs::path& path)
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

    inline bool RotatePrimaryToBackup(const fs::path& primary, const fs::path& backup, std::string& error)
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
} // namespace SparkEditor::RecoveryDetail
