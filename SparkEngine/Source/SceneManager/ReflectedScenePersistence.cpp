#include "SceneManager/ReflectedSceneSerializer.h"

#include "Core/Reflection.h"
#include "Engine/ECS/Components.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <filesystem>
#include <new>
#include <string>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Spark
{
    namespace
    {
        /// Read a scene document of at most kMaxSceneDocumentBytes. The size is
        /// checked before anything is allocated, and the read itself stops one byte
        /// past the limit, so a file that grows between the size check and the read
        /// is rejected too. On failure @p reason names the cause.
        bool ReadTextFile(const std::filesystem::path& path, std::string& text, std::string& reason)
        {
            text.clear();
            std::error_code statusError;
            if (!std::filesystem::is_regular_file(path, statusError))
            {
                reason = "file is not a regular file";
                return false;
            }
            const std::uintmax_t declaredSize = std::filesystem::file_size(path, statusError);
            if (statusError)
            {
                reason = "file size could not be read: " + statusError.message();
                return false;
            }
            if (declaredSize > kMaxSceneDocumentBytes)
            {
                reason = std::format("file is {} bytes; the scene size limit is {} bytes", declaredSize,
                                     kMaxSceneDocumentBytes);
                return false;
            }

            std::ifstream input(path, std::ios::binary);
            if (!input.is_open())
            {
                reason = "file could not be read";
                return false;
            }
            try
            {
                text.reserve(static_cast<size_t>(declaredSize));
                constexpr size_t kChunkBytes = size_t{1024} * 1024u;
                std::string chunk(kChunkBytes, '\0');
                while (input)
                {
                    input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                    const auto got = static_cast<size_t>(input.gcount());
                    if (got == 0)
                        break;
                    if (static_cast<uint64_t>(text.size()) + got > kMaxSceneDocumentBytes)
                    {
                        text.clear();
                        reason = std::format("file grew past the scene size limit of {} bytes while being read",
                                             kMaxSceneDocumentBytes);
                        return false;
                    }
                    text.append(chunk.data(), got);
                }
            }
            catch (const std::bad_alloc&)
            {
                text.clear();
                reason = "not enough memory to read the file";
                return false;
            }
            if (input.bad())
            {
                text.clear();
                reason = "file could not be read";
                return false;
            }
            return true;
        }

        bool FlushFileDurably(const std::filesystem::path& path, std::error_code& error)
        {
#if defined(_WIN32)
            const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                              FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                error = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
                return false;
            }

            const bool flushed = ::FlushFileBuffers(file) != FALSE;
            const DWORD flushError = flushed ? ERROR_SUCCESS : ::GetLastError();
            ::CloseHandle(file);
            if (!flushed)
            {
                error = std::error_code(static_cast<int>(flushError), std::system_category());
                return false;
            }
            return true;
#else
            const int file = ::open(path.c_str(), O_RDONLY);
            if (file < 0)
            {
                error = std::error_code(errno, std::generic_category());
                return false;
            }

            const bool flushed = ::fsync(file) == 0;
            const int flushError = flushed ? 0 : errno;
            ::close(file);
            if (!flushed)
            {
                error = std::error_code(flushError, std::generic_category());
                return false;
            }
            return true;
#endif
        }

        bool ReplaceFileAtomically(const std::filesystem::path& temporary, const std::filesystem::path& destination,
                                   std::error_code& error)
        {
#if defined(_WIN32)
            if (::MoveFileExW(temporary.c_str(), destination.c_str(),
                              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                return true;
            }
            error = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
            return false;
#else
            std::filesystem::rename(temporary, destination, error);
            if (error)
                return false;

            const std::filesystem::path directory = destination.has_parent_path() ? destination.parent_path() : ".";
#if defined(O_DIRECTORY)
            const int directoryFile = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
#else
            const int directoryFile = ::open(directory.c_str(), O_RDONLY);
#endif
            if (directoryFile < 0)
            {
                error = std::error_code(errno, std::generic_category());
                return false;
            }
            const bool flushed = ::fsync(directoryFile) == 0;
            const int flushError = flushed ? 0 : errno;
            ::close(directoryFile);
            if (!flushed)
            {
                error = std::error_code(flushError, std::generic_category());
                return false;
            }
            return true;
#endif
        }

        bool WriteDurableText(const std::filesystem::path& path, const std::string& text, std::error_code& error)
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output.is_open())
                return false;
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
            output.close();
            if (output.fail())
                return false;
            return FlushFileDurably(path, error);
        }

        void RemoveFileNoThrow(const std::filesystem::path& path)
        {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } // namespace

    bool SaveWorld(const World& world, const std::string& path, std::string* error)
    {
        if (error)
        {
            error->clear();
        }

        // Serialize before touching any file. A world the reader would refuse (a
        // NaN/Inf field, or a document over the size or value caps) must fail here:
        // writing it would make LoadWorld reject the new primary and silently fall
        // back to the older .bak, discarding everything since that save.
        std::string serialized;
        std::string serializeReason;
        if (!TrySerializeWorld(world, serialized, &serializeReason))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "[ReflectedScene] refusing to save %s: %s", path.c_str(),
                            serializeReason.c_str());
            if (error)
            {
                *error = "Scene '" + path + "' was not saved: " + serializeReason + ".";
            }
            return false;
        }

        const std::filesystem::path destination = std::filesystem::u8path(path);
        std::filesystem::path temporary = destination;
        temporary += ".tmp";
        std::filesystem::path backup = destination;
        backup += ".bak";
        std::filesystem::path backupTemporary = backup;
        backupTemporary += ".tmp";

        RemoveFileNoThrow(temporary);
        RemoveFileNoThrow(backupTemporary);

        const auto fail = [&](const char* stage, const std::error_code& ioError)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "[ReflectedScene] %s failed for %s: %s", stage, path.c_str(),
                           ioError.message().c_str());
            if (error)
            {
                *error = "Scene '" + path + "' was not saved: " + stage + " failed: " + ioError.message() + ".";
            }
            return false;
        };

        std::error_code ioError;
        if (!WriteDurableText(temporary, serialized, ioError))
        {
            RemoveFileNoThrow(temporary);
            return fail("durable staging write", ioError);
        }

        // Preserve the previous image only when it is a loadable scene. A
        // corrupt destination must never displace the last known-good backup.
        // A destination over the size limit is not loadable, so it is treated like
        // any other invalid previous image: never read whole, never kept as .bak.
        std::string previous;
        std::string previousReason;
        if (ReadTextFile(destination, previous, previousReason))
        {
            World validationWorld(World::EntityEventCleanupMode::Suppressed);
            if (DeserializeInto(validationWorld, previous))
            {
                if (!WriteDurableText(backupTemporary, previous, ioError) ||
                    !ReplaceFileAtomically(backupTemporary, backup, ioError))
                {
                    RemoveFileNoThrow(temporary);
                    RemoveFileNoThrow(backupTemporary);
                    return fail("previous-good backup", ioError);
                }
            }
        }

        if (!ReplaceFileAtomically(temporary, destination, ioError))
        {
            RemoveFileNoThrow(temporary);
            return fail("atomic replace", ioError);
        }
        return true;
    }

    bool LoadWorld(World& world, const std::string& path, std::string* error)
    {
        if (error)
            error->clear();

        const std::filesystem::path primary = std::filesystem::u8path(path);
        std::filesystem::path backup = primary;
        backup += ".bak";

        const auto loadCandidate = [&](const std::filesystem::path& candidatePath, std::string& reason) -> bool
        {
            std::error_code existsError;
            if (!std::filesystem::exists(candidatePath, existsError))
            {
                reason = "file does not exist";
                return false;
            }

            std::string text;
            if (!ReadTextFile(candidatePath, text, reason))
            {
                return false;
            }

            // Deserialize into an isolated world first. A malformed document
            // can fail after creating entities or components; applying that
            // attempt directly to the caller would contaminate a later backup
            // recovery (and could leave a live editor document partially read).
            World staged;
            if (!DeserializeInto(staged, text, SceneDeserializeMode::Permissive, &reason))
                return false;

            // The editor and runtime replace the loaded document. Install only
            // a candidate that completed successfully, so a failed primary or
            // backup leaves the caller's existing world untouched.
            world.GetRegistry() = std::move(staged.GetRegistry());
            return true;
        };

        std::string primaryReason;
        if (loadCandidate(primary, primaryReason))
            return true;

        std::string backupReason;
        if (!loadCandidate(backup, backupReason))
        {
            if (error)
            {
                *error = "Scene '" + path + "' was rejected: " + primaryReason + ". Previous-good backup '" + path +
                         ".bak' was not usable: " + backupReason + ".";
            }
            return false;
        }

        SPARK_LOG_WARN(Spark::LogCategory::Core, "[ReflectedScene] recovering %s from previous-good backup (%s)",
                       path.c_str(), primaryReason.c_str());
        return true;
    }
} // namespace Spark
