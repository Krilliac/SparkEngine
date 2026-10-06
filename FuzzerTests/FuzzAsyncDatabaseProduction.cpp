/**
 * @file FuzzAsyncDatabaseProduction.cpp
 * @brief libc++-compiled production adapter for the AsyncDatabase store-file harness.
 *
 * AsyncDatabasePool::Open opens every pooled SQLiteConnection on one store file, and
 * SQLiteConnection::Open loads that file (the MMO character, guild and world records
 * live in it) before the server accepts a player. The input is written as the store
 * file and opened under a 64 KiB budget. A violated contract aborts so libFuzzer
 * records a crash:
 *  - Open accepts exactly the files an independent model of the documented format
 *    accepts: newline-terminated "key<TAB>value" records, verbatim in a legacy file,
 *    escape-decoded (\\ \t \n \r only) after a "#!spark-kv-v2" first line, no repeated
 *    key, and no store whose canonical revision exceeds the budget (the next write
 *    could never be published),
 *  - a rejected Open leaves the connection closed and the file byte-for-byte intact,
 *  - an accepted store commits (republishes) successfully, the published revision
 *    holds exactly the model's records, and reopening that revision reproduces them.
 */

#include "FuzzAsyncDatabaseProduction.h"

#include "Engine/Persistence/AsyncDatabase.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace
{
    namespace fs = std::filesystem;
    using Spark::Persistence::SQLiteConnection;
    using Records = std::map<std::string, std::string>;

    constexpr std::size_t kStoreBudget = 64u * 1024u;
    constexpr std::size_t kMaxInputBytes = kStoreBudget + 1u;
    constexpr std::string_view kMarker = "#!spark-kv-v2";
    std::atomic<std::uint64_t> s_directoryCounter{0};

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzAsyncDatabase: %s\n", what);
        std::abort();
    }

    /// A fresh directory for one input; removed with its contents on scope exit.
    class ScopedDirectory
    {
      public:
        ScopedDirectory()
        {
            const auto suffix = s_directoryCounter.fetch_add(1, std::memory_order_relaxed);
            m_path = fs::temp_directory_path() /
                     ("spark-fuzz-async-database-" + std::to_string(getpid()) + "-" + std::to_string(suffix));
            std::error_code error;
            fs::remove_all(m_path, error);
            m_created = fs::create_directory(m_path, error) && !error;
        }
        ~ScopedDirectory()
        {
            std::error_code ignored;
            fs::remove_all(m_path, ignored);
        }
        ScopedDirectory(const ScopedDirectory&) = delete;
        ScopedDirectory& operator=(const ScopedDirectory&) = delete;

        bool Created() const { return m_created; }
        const fs::path& Path() const { return m_path; }

      private:
        fs::path m_path;
        bool m_created = false;
    };

    std::string ReadAll(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    bool WriteAll(const fs::path& path, std::string_view bytes)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!bytes.empty())
        {
            file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        file.close();
        return static_cast<bool>(file);
    }

    std::optional<std::string> Unescape(std::string_view field)
    {
        std::string out;
        for (std::size_t i = 0; i < field.size(); ++i)
        {
            if (field[i] != '\\')
            {
                out += field[i];
                continue;
            }
            if (++i == field.size())
            {
                return std::nullopt;
            }
            switch (field[i])
            {
            case 't':
                out += '\t';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case '\\':
                out += '\\';
                break;
            default:
                return std::nullopt;
            }
        }
        return out;
    }

    /// Bytes one field takes in the escaped revision FlushToDisk publishes.
    std::size_t EscapedSize(const std::string& field)
    {
        std::size_t size = field.size();
        for (const char c : field)
        {
            if (c == '\\' || c == '\t' || c == '\n' || c == '\r')
            {
                ++size;
            }
        }
        return size;
    }

    /// Independent model of the POSIX store reader and its budget.
    std::optional<Records> ModelLoad(std::string_view bytes)
    {
        if (bytes.size() > kStoreBudget)
        {
            return std::nullopt;
        }
        Records records;
        bool escaped = false;
        std::size_t lineNumber = 0;
        std::size_t canonicalBytes = kMarker.size() + 1u;
        while (!bytes.empty())
        {
            const std::size_t end = bytes.find('\n');
            if (end == std::string_view::npos)
            {
                return std::nullopt; // not newline-terminated
            }
            const std::string_view line = bytes.substr(0, end);
            bytes.remove_prefix(end + 1);
            if (++lineNumber == 1 && line == kMarker)
            {
                escaped = true;
                continue;
            }
            const std::size_t tab = line.find('\t');
            if (tab == std::string_view::npos)
            {
                return std::nullopt;
            }
            std::optional<std::string> key = std::string(line.substr(0, tab));
            std::optional<std::string> value = std::string(line.substr(tab + 1));
            if (escaped)
            {
                key = Unescape(*key);
                value = Unescape(*value);
                if (!key || !value)
                {
                    return std::nullopt;
                }
            }
            canonicalBytes += EscapedSize(*key) + 1u + EscapedSize(*value) + 1u;
            if (!records.emplace(std::move(*key), std::move(*value)).second)
            {
                return std::nullopt;
            }
        }
        if (canonicalBytes > kStoreBudget)
        {
            return std::nullopt;
        }
        return records;
    }

    /// Commit an empty transaction (which republishes the whole store) and read the revision back.
    Records Republish(SQLiteConnection& connection, const fs::path& store)
    {
        if (!connection.BeginTransaction() || !connection.CommitTransaction())
        {
            InvariantFailure("an accepted store cannot be republished");
        }
        const std::string revision = ReadAll(store);
        if (!revision.starts_with(std::string(kMarker) + "\n"))
        {
            InvariantFailure("the published revision does not start with the escaped-format marker");
        }
        const std::optional<Records> published = ModelLoad(revision);
        if (!published)
        {
            InvariantFailure("the published revision is not a well-formed store");
        }
        return *published;
    }
} // namespace

extern "C" int SparkFuzzOpenAsyncDatabase(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    ScopedDirectory directory;
    if (!directory.Created())
    {
        return 0;
    }
    const fs::path store = directory.Path() / "records.db";
    if (!WriteAll(store, input))
    {
        return 0;
    }

    const std::optional<Records> expected = ModelLoad(input);
    {
        SQLiteConnection connection(kStoreBudget);
        const bool accepted = connection.Open(store.string());
        if (accepted != expected.has_value())
        {
            InvariantFailure(accepted ? "Open accepted a store the format model rejects"
                                      : "Open rejected a store the format model accepts");
        }
        if (!accepted)
        {
            if (connection.IsOpen())
            {
                InvariantFailure("a rejected Open left the connection open");
            }
            if (ReadAll(store) != input)
            {
                InvariantFailure("a rejected Open modified the store file");
            }
            return 0;
        }
        if (Republish(connection, store) != *expected)
        {
            InvariantFailure("the republished store differs from the records the file held");
        }
        connection.Close();
    }

    SQLiteConnection reopened(kStoreBudget);
    if (!reopened.Open(store.string()))
    {
        InvariantFailure("a published revision does not open again");
    }
    if (Republish(reopened, store) != *expected)
    {
        InvariantFailure("reopening the published revision changed its records");
    }
    reopened.Close();
    return 0;
}
