#include "SceneManager/ReflectedSceneSerializer.h"

#include "Core/Reflection.h"
#include "Engine/ECS/Components.h"
#include "Utils/LogMacros.h"

#include <fstream>
#include <filesystem>
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
        bool ReadTextFile(const std::filesystem::path& path, std::string& text)
        {
            std::ifstream input(path, std::ios::binary);
            if (!input.is_open())
                return false;
            text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
            return input.good() || input.eof();
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

    bool SaveWorld(const World& world, const std::string& path)
    {
        const std::filesystem::path destination = std::filesystem::u8path(path);
        std::filesystem::path temporary = destination;
        temporary += ".tmp";
        std::filesystem::path backup = destination;
        backup += ".bak";
        std::filesystem::path backupTemporary = backup;
        backupTemporary += ".tmp";

        RemoveFileNoThrow(temporary);
        RemoveFileNoThrow(backupTemporary);

        const std::string serialized = SerializeWorld(world);
        std::error_code error;
        if (!WriteDurableText(temporary, serialized, error))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "[ReflectedScene] durable staging write failed for %s: %s",
                           path.c_str(), error.message().c_str());
            RemoveFileNoThrow(temporary);
            return false;
        }

        // Preserve the previous image only when it is a loadable scene. A
        // corrupt destination must never displace the last known-good backup.
        std::string previous;
        if (ReadTextFile(destination, previous))
        {
            World validationWorld(World::EntityEventCleanupMode::Suppressed);
            if (DeserializeInto(validationWorld, previous))
            {
                if (!WriteDurableText(backupTemporary, previous, error) ||
                    !ReplaceFileAtomically(backupTemporary, backup, error))
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Core, "[ReflectedScene] previous-good backup failed for %s: %s",
                                   path.c_str(), error.message().c_str());
                    RemoveFileNoThrow(temporary);
                    RemoveFileNoThrow(backupTemporary);
                    return false;
                }
            }
        }

        if (!ReplaceFileAtomically(temporary, destination, error))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "[ReflectedScene] atomic replace failed for %s: %s", path.c_str(),
                           error.message().c_str());
            RemoveFileNoThrow(temporary);
            return false;
        }
        return true;
    }

    bool LoadWorld(World& world, const std::string& path)
    {
        const std::filesystem::path primary = std::filesystem::u8path(path);
        std::filesystem::path backup = primary;
        backup += ".bak";

        const auto loadCandidate = [&](const std::filesystem::path& candidatePath) -> bool
        {
            std::string text;
            if (!ReadTextFile(candidatePath, text))
                return false;

            // Deserialize into an isolated world first. A malformed document
            // can fail after creating entities or components; applying that
            // attempt directly to the caller would contaminate a later backup
            // recovery (and could leave a live editor document partially read).
            World staged;
            if (!DeserializeInto(staged, text))
                return false;

            // The editor and runtime replace the loaded document. Install only
            // a candidate that completed successfully, so a failed primary or
            // backup leaves the caller's existing world untouched.
            world.GetRegistry() = std::move(staged.GetRegistry());
            return true;
        };

        if (loadCandidate(primary))
            return true;

        if (!loadCandidate(backup))
            return false;

        SPARK_LOG_WARN(Spark::LogCategory::Core, "[ReflectedScene] recovering %s from previous-good backup",
                       path.c_str());
        return true;
    }
} // namespace Spark
