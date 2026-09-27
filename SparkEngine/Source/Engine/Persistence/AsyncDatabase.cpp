/**
 * @file AsyncDatabase.cpp
 * @brief Implementation of the async database persistence layer.
 */

#include "AsyncDatabase.h"
#include "Engine/SaveSystem/SaveFileDurability.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cerrno>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace Spark::Persistence
{

    // ============================================================================
    // QueryRow
    // ============================================================================

    int64_t QueryRow::GetInt(size_t col) const
    {
        if (col >= columns.size())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "QueryRow::GetInt column %zu out of range (size=%zu)", col,
                            columns.size());
            return 0;
        }
        if (const auto* v = std::get_if<int64_t>(&columns[col]))
            return *v;
        // Allow safe widening from double if the storage happens to be floating point.
        if (const auto* v = std::get_if<double>(&columns[col]))
            return static_cast<int64_t>(*v);
        SPARK_LOG_WARN(Spark::LogCategory::Core, "QueryRow::GetInt column %zu holds a non-integer variant (index=%zu)",
                       col, columns[col].index());
        return 0;
    }

    double QueryRow::GetDouble(size_t col) const
    {
        if (col >= columns.size())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "QueryRow::GetDouble column %zu out of range (size=%zu)", col,
                            columns.size());
            return 0.0;
        }
        if (const auto* v = std::get_if<double>(&columns[col]))
            return *v;
        if (const auto* v = std::get_if<int64_t>(&columns[col]))
            return static_cast<double>(*v);
        SPARK_LOG_WARN(Spark::LogCategory::Core,
                       "QueryRow::GetDouble column %zu holds a non-numeric variant (index=%zu)", col,
                       columns[col].index());
        return 0.0;
    }

    const std::string& QueryRow::GetString(size_t col) const
    {
        static const std::string empty;
        if (col >= columns.size())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "QueryRow::GetString column %zu out of range (size=%zu)", col,
                            columns.size());
            return empty;
        }
        if (const auto* v = std::get_if<std::string>(&columns[col]))
            return *v;
        SPARK_LOG_WARN(Spark::LogCategory::Core,
                       "QueryRow::GetString column %zu holds a non-string variant (index=%zu)", col,
                       columns[col].index());
        return empty;
    }

    bool QueryRow::IsNull(size_t col) const
    {
        if (col >= columns.size())
        {
            return true;
        }
        return std::holds_alternative<std::monostate>(columns[col]);
    }

    // ============================================================================
    // PreparedStatementData
    // ============================================================================

    void PreparedStatementData::SetInt(size_t index, int64_t value)
    {
        if (index >= params.size())
        {
            params.resize(index + 1);
        }
        params[index].value = value;
    }

    void PreparedStatementData::SetDouble(size_t index, double value)
    {
        if (index >= params.size())
        {
            params.resize(index + 1);
        }
        params[index].value = value;
    }

    void PreparedStatementData::SetString(size_t index, const std::string& value)
    {
        if (index >= params.size())
        {
            params.resize(index + 1);
        }
        params[index].value = value;
    }

    void PreparedStatementData::SetNull(size_t index)
    {
        if (index >= params.size())
        {
            params.resize(index + 1);
        }
        params[index].value = std::monostate{};
    }

    void PreparedStatementData::ClearParams()
    {
        params.clear();
    }

    // ============================================================================
    // Transaction
    // ============================================================================

    void Transaction::Append(PreparedStatementID stmtId, std::vector<PreparedStatementParam> params)
    {
        queries.emplace_back(stmtId, std::move(params));
    }

    // ============================================================================
    // SQLiteConnection — file-based JSON key-value fallback
    // ============================================================================

    namespace
    {
        // The KV file stores one "key\tvalue\n" record per line, but SET supports values
        // with embedded newlines (and tabs). Escape the delimiters (and the escape
        // character itself) on write and reverse it on load so multi-line values survive
        // a flush/reload round trip instead of being silently truncated.
        std::string EscapeKVField(const std::string& field)
        {
            std::string escaped;
            escaped.reserve(field.size());
            for (char c : field)
            {
                switch (c)
                {
                case '\\':
                    escaped += "\\\\";
                    break;
                case '\t':
                    escaped += "\\t";
                    break;
                case '\n':
                    escaped += "\\n";
                    break;
                case '\r':
                    // Escaped so a CR can never be mistaken for a legacy CRLF line ending.
                    escaped += "\\r";
                    break;
                default:
                    escaped += c;
                    break;
                }
            }
            return escaped;
        }

        // First line of files written in the escaped format. Files without it
        // predate escaping and store raw bytes — they must load verbatim, or a
        // legacy value like "C:\temp" would decode its "\t" into a tab.
        constexpr const char* kKVFormatMarker = "#!spark-kv-v2";

        /// Reverse EscapeKVField. Only the sequences the writer produces are accepted: any
        /// other escape, or a trailing lone backslash, means the record is damaged.
        bool UnescapeKVField(const std::string& field, std::string& out)
        {
            out.clear();
            out.reserve(field.size());
            for (size_t i = 0; i < field.size(); ++i)
            {
                if (field[i] != '\\')
                {
                    out += field[i];
                    continue;
                }
                if (i + 1 >= field.size())
                    return false;
                ++i;
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
                    return false;
                }
            }
            return true;
        }
    } // namespace

    SQLiteConnection::~SQLiteConnection()
    {
        if (m_open)
        {
            Close();
        }
    }

    bool SQLiteConnection::Open(const std::string& connectionString)
    {
        if (m_open)
        {
            return false;
        }

        m_dbPath = connectionString;

        // Ensure parent directory exists
        auto parentPath = std::filesystem::path(m_dbPath).parent_path();
        if (!parentPath.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(parentPath, ec);
            if (ec)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Core, "AsyncDatabase: failed to create directory '%s': %s",
                                parentPath.string().c_str(), ec.message().c_str());
                return false;
            }
        }

        // One authority per store: a second holder would keep its own snapshot and each
        // whole-store flush would silently erase the other's commits.
        if (!AcquireAuthorityLock())
        {
            return false;
        }

        if (!LoadFromDisk())
        {
            // Never run with an empty or partial store over a file that exists: the next
            // flush would atomically replace every record that was not loaded.
            m_kvStore.clear();
            ReleaseAuthorityLock();
            return false;
        }

        m_open = true;
        return true;
    }

    bool SQLiteConnection::AcquireAuthorityLock()
    {
        const std::filesystem::path lockPath(m_dbPath + ".lock");
#ifdef _WIN32
        // No sharing: while this handle is open, any other open of the lock file fails.
        HANDLE handle = ::CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            const DWORD error = ::GetLastError();
            SPARK_LOG_ERROR(Spark::LogCategory::Core,
                            "AsyncDatabase: '%s' is already open by another connection or process (lock '%s': %s)",
                            m_dbPath.c_str(), lockPath.string().c_str(),
                            std::system_category().message(static_cast<int>(error)).c_str());
            return false;
        }
        m_lockHandle = handle;
#else
        int flags = O_RDWR | O_CREAT;
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        const int fd = ::open(lockPath.c_str(), flags, 0600);
        if (fd < 0)
        {
            const int error = errno;
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "AsyncDatabase: cannot open lock '%s': %s",
                            lockPath.string().c_str(), std::strerror(error));
            return false;
        }
        // flock locks belong to the open file description, so a second open of the same
        // store fails here even inside one process.
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0)
        {
            const int error = errno;
            ::close(fd);
            SPARK_LOG_ERROR(Spark::LogCategory::Core,
                            "AsyncDatabase: '%s' is already open by another connection or process (lock '%s': %s)",
                            m_dbPath.c_str(), lockPath.string().c_str(), std::strerror(error));
            return false;
        }
        m_lockFd = fd;
#endif
        return true;
    }

    void SQLiteConnection::ReleaseAuthorityLock() noexcept
    {
#ifdef _WIN32
        if (m_lockHandle != nullptr)
        {
            ::CloseHandle(static_cast<HANDLE>(m_lockHandle));
            m_lockHandle = nullptr;
        }
#else
        if (m_lockFd >= 0)
        {
            // Closing the descriptor releases the flock. The lock file is deliberately not
            // unlinked: unlinking would let a new opener lock a fresh inode while an older
            // opener still holds the unlinked one.
            ::close(m_lockFd);
            m_lockFd = -1;
        }
#endif
    }

    void SQLiteConnection::Close()
    {
        if (!m_open)
        {
            return;
        }

        if (!FlushToDisk())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core,
                            "AsyncDatabase: final flush of '%s' failed on close; the file keeps its last "
                            "successfully published revision",
                            m_dbPath.c_str());
        }
        m_kvStore.clear();
        m_preparedSQL.clear();
        m_open = false;
        ReleaseAuthorityLock();
    }

    bool SQLiteConnection::PrepareStatement(PreparedStatementID id, const std::string& sql)
    {
        m_preparedSQL[id] = sql;
        return true;
    }

    QueryResult SQLiteConnection::Execute(PreparedStatementID id, const std::vector<PreparedStatementParam>& params)
    {
        auto it = m_preparedSQL.find(id);
        if (it == m_preparedSQL.end())
        {
            QueryResult result;
            result.success = false;
            result.errorMessage = "Unknown prepared statement ID: " + std::to_string(id);
            return result;
        }

        // Pre-compute the escaped replacement text for every bound parameter.
        // Params are referenced as ?0, ?1, ?2... in the prepared SQL string.
        std::vector<std::string> replacements(params.size());
        for (size_t i = 0; i < params.size(); ++i)
        {
            std::visit(
                [&](auto&& arg)
                {
                    using T = std::decay_t<decltype(arg)>;
                    if constexpr (std::is_same_v<T, std::monostate>)
                    {
                        replacements[i] = "NULL";
                    }
                    else if constexpr (std::is_same_v<T, int64_t>)
                    {
                        replacements[i] = std::to_string(arg);
                    }
                    else if constexpr (std::is_same_v<T, double>)
                    {
                        replacements[i] = std::to_string(arg);
                    }
                    else if constexpr (std::is_same_v<T, std::string>)
                    {
                        // Escape single quotes to prevent SQL injection
                        std::string escaped;
                        escaped.reserve(arg.size() + 2);
                        escaped += '\'';
                        for (char c : arg)
                        {
                            if (c == '\'')
                                escaped += "''";
                            else
                                escaped += c;
                        }
                        escaped += '\'';
                        replacements[i] = std::move(escaped);
                    }
                    else if constexpr (std::is_same_v<T, std::vector<uint8_t>>)
                    {
                        replacements[i] = "[blob:" + std::to_string(arg.size()) + "]";
                    }
                },
                params[i].value);
        }

        // Single left-to-right pass over the template, emitting into a fresh output
        // string. Each "?<digits>" token is replaced by its bound parameter. Because the
        // output is never re-scanned, a substituted value that itself contains "?N" can
        // never be corrupted, and a placeholder appearing multiple times is replaced at
        // every occurrence. A "?<digits>" with no matching bound parameter is emitted
        // verbatim (matching the previous behavior for unbound placeholders).
        const std::string& sqlTemplate = it->second;
        std::string sql;
        sql.reserve(sqlTemplate.size());
        for (size_t pos = 0; pos < sqlTemplate.size();)
        {
            if (sqlTemplate[pos] == '?' && pos + 1 < sqlTemplate.size() &&
                std::isdigit(static_cast<unsigned char>(sqlTemplate[pos + 1])))
            {
                size_t digitsBegin = pos + 1;
                size_t digitsEnd = digitsBegin;
                while (digitsEnd < sqlTemplate.size() &&
                       std::isdigit(static_cast<unsigned char>(sqlTemplate[digitsEnd])))
                {
                    ++digitsEnd;
                }

                size_t index = 0;
                auto [ptr, ec] =
                    std::from_chars(sqlTemplate.data() + digitsBegin, sqlTemplate.data() + digitsEnd, index);
                (void)ptr;
                if (ec == std::errc() && index < replacements.size())
                {
                    sql += replacements[index];
                }
                else
                {
                    // Unbound or out-of-range placeholder: keep it verbatim.
                    sql.append(sqlTemplate, pos, digitsEnd - pos);
                }
                pos = digitsEnd;
            }
            else
            {
                sql += sqlTemplate[pos];
                ++pos;
            }
        }

        return ExecuteRaw(sql);
    }

    QueryResult SQLiteConnection::ExecuteRaw(const std::string& sql)
    {
        QueryResult result;

        // Minimal SQL-like operations on the key-value store.
        // Supports: SET key value, GET key, DELETE key, KEYS [prefix]
        std::istringstream stream(sql);
        std::string command;
        stream >> command;

        // Normalize to uppercase for comparison
        std::string upperCmd = command;
        std::transform(upperCmd.begin(), upperCmd.end(), upperCmd.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

        if (upperCmd == "SET")
        {
            std::string key;
            stream >> key;
            std::string value;
            {
                // Read the remainder of the command verbatim (not just up to the first
                // embedded '\n') so values containing embedded newlines are not truncated.
                std::ostringstream valueStream;
                valueStream << stream.rdbuf();
                value = valueStream.str();
            }

            // Trim leading whitespace from value
            if (!value.empty() && value[0] == ' ')
            {
                value = value.substr(1);
            }

            // Execute() substitutes string parameters as single-quoted SQL literals with
            // doubled apostrophes, but this fallback store is not SQL and would otherwise
            // keep the quoting verbatim, leaking it back out of every GET. If the entire
            // value is one well-formed quoted literal, decode it; anything else (multiple
            // tokens, stray quotes) is raw text and is stored verbatim.
            if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'')
            {
                std::string decoded;
                decoded.reserve(value.size() - 2);
                bool wellFormed = true;
                for (size_t i = 1; i + 1 < value.size(); ++i)
                {
                    if (value[i] == '\'')
                    {
                        // An interior apostrophe must be the first half of a doubled pair.
                        if (i + 2 < value.size() && value[i + 1] == '\'')
                        {
                            decoded += '\'';
                            ++i;
                        }
                        else
                        {
                            wellFormed = false;
                            break;
                        }
                    }
                    else
                    {
                        decoded += value[i];
                    }
                }
                if (wellFormed)
                {
                    value = std::move(decoded);
                }
            }

            std::optional<std::string> previous;
            if (const auto existing = m_kvStore.find(key); existing != m_kvStore.end())
            {
                previous = existing->second;
            }
            m_kvStore[key] = value;

            // Persist non-transactional writes immediately so data survives a crash and
            // is visible on the next reopen. Writes issued inside a transaction are
            // deferred to CommitTransaction so RollbackTransaction can discard them.
            // A write that was not published is undone and reported as failed: callers
            // such as character-ID allocation rely on success meaning durable.
            if (!m_inTransaction && !FlushToDisk())
            {
                if (previous)
                {
                    m_kvStore[key] = std::move(*previous);
                }
                else
                {
                    m_kvStore.erase(key);
                }
                result.success = false;
                result.errorMessage = "SET was not persisted: publishing '" + m_dbPath + "' failed";
                return result;
            }
            result.success = true;
            result.affectedRows = 1;
        }
        else if (upperCmd == "GET")
        {
            std::string key;
            stream >> key;

            auto it = m_kvStore.find(key);
            if (it != m_kvStore.end())
            {
                QueryRow row;
                row.columns.emplace_back(it->second);
                result.rows.push_back(std::move(row));
            }
            result.success = true;
        }
        else if (upperCmd == "DELETE")
        {
            std::string key;
            stream >> key;

            auto erased = m_kvStore.extract(key);
            const bool removed = !erased.empty();

            // Persist the deletion immediately (see the SET branch for rationale).
            if (!m_inTransaction && removed && !FlushToDisk())
            {
                m_kvStore.insert(std::move(erased));
                result.success = false;
                result.errorMessage = "DELETE was not persisted: publishing '" + m_dbPath + "' failed";
                return result;
            }
            result.success = true;
            result.affectedRows = removed ? 1 : 0;
        }
        else if (upperCmd == "KEYS")
        {
            std::string prefix;
            stream >> prefix;

            for (const auto& [key, val] : m_kvStore)
            {
                if (prefix.empty() || key.starts_with(prefix))
                {
                    QueryRow row;
                    row.columns.emplace_back(key);
                    result.rows.push_back(std::move(row));
                }
            }
            result.success = true;
        }
        else
        {
            result.success = false;
            result.errorMessage = "Unsupported command in fallback store: " + command;
        }

        return result;
    }

    bool SQLiteConnection::BeginTransaction()
    {
        if (m_inTransaction)
        {
            return false;
        }
        m_transactionSnapshot = m_kvStore;
        m_inTransaction = true;
        return true;
    }

    bool SQLiteConnection::CommitTransaction()
    {
        if (!m_inTransaction)
        {
            return false;
        }

        if (!FlushToDisk())
        {
            // A transaction is not committed until its durable revision has
            // replaced the destination.  Restore the snapshot when that
            // boundary fails so callers cannot observe an unpersisted commit.
            m_kvStore = std::move(m_transactionSnapshot);
            m_transactionSnapshot.clear();
            m_inTransaction = false;
            return false;
        }

        m_inTransaction = false;
        m_transactionSnapshot.clear();
        return true;
    }

    bool SQLiteConnection::RollbackTransaction()
    {
        if (!m_inTransaction)
        {
            return false;
        }
        m_kvStore = std::move(m_transactionSnapshot);
        m_inTransaction = false;
        return true;
    }

    bool SQLiteConnection::FlushToDisk()
    {
        // Never truncate the live store: a crash or a write error midway through would
        // destroy every key. The complete revision is built in memory, staged in a sibling
        // temp file created exclusively (never through a link planted at that name),
        // flushed, and published by atomic replace plus, on POSIX, a directory sync
        // (DATA-120).
        //
        // The writer enforces the same budget LoadFromDisk does. Escaping can double a
        // value, so a run of individually accepted SETs could otherwise publish a store the
        // next Open() refuses, leaving the server unable to start. The check runs while the
        // revision is built so an oversized store is refused before it is fully materialized.
        std::string revision;
        revision += kKVFormatMarker;
        revision += '\n';
        for (const auto& [key, value] : m_kvStore)
        {
            revision += EscapeKVField(key);
            revision += '\t';
            revision += EscapeKVField(value);
            revision += '\n';
            if (static_cast<std::uintmax_t>(revision.size()) > m_maxStoreFileBytes)
            {
                break;
            }
        }
        if (static_cast<std::uintmax_t>(revision.size()) > m_maxStoreFileBytes)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core,
                            "AsyncDatabase: refusing to publish '%s' (%zu entries): the revision exceeds the "
                            "%ju-byte store budget Open() enforces; the write is rejected",
                            m_dbPath.c_str(), m_kvStore.size(), static_cast<std::uintmax_t>(m_maxStoreFileBytes));
            return false;
        }

        std::error_code error;
        if (!Spark::SaveFileDurability::WriteFileAtomically(std::filesystem::path(m_dbPath), revision,
                                                            /*retainBackup*/ false, error))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "AsyncDatabase: failed to publish '%s' (%zu entries): %s",
                            m_dbPath.c_str(), m_kvStore.size(), error.message().c_str());
            return false;
        }
        if (error)
        {
            // Published: the destination names the new revision. Only the directory sync
            // that makes the rename survive a power loss failed.
            SPARK_LOG_WARN(Spark::LogCategory::Core,
                           "AsyncDatabase: '%s' was published but the directory sync failed (%s); the revision may "
                           "not survive a power loss",
                           m_dbPath.c_str(), error.message().c_str());
        }
        return true;
    }

    bool SQLiteConnection::LoadFromDisk()
    {
        m_kvStore.clear();

        const std::filesystem::path path(m_dbPath);
        std::error_code statusError;
        const std::filesystem::file_status status = std::filesystem::status(path, statusError);
        if (status.type() == std::filesystem::file_type::not_found)
        {
            SPARK_LOG_INFO(Spark::LogCategory::Core, "AsyncDatabase: '%s' not found; starting with an empty store",
                           m_dbPath.c_str());
            return true;
        }
        if (statusError || status.type() != std::filesystem::file_type::regular)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core,
                            "AsyncDatabase: '%s' exists but is not a readable regular file%s%s", m_dbPath.c_str(),
                            statusError ? ": " : "", statusError ? statusError.message().c_str() : "");
            return false;
        }

        std::error_code sizeError;
        const std::uintmax_t size = std::filesystem::file_size(path, sizeError);
        if (sizeError || size > m_maxStoreFileBytes)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core,
                            "AsyncDatabase: refusing to load '%s': size %ju exceeds the %ju-byte store budget%s%s",
                            m_dbPath.c_str(), static_cast<std::uintmax_t>(sizeError ? 0 : size),
                            static_cast<std::uintmax_t>(m_maxStoreFileBytes), sizeError ? ": " : "",
                            sizeError ? sizeError.message().c_str() : "");
            return false;
        }

        // Binary: a text-mode read on Windows stops at a 0x1A byte inside a value.
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "AsyncDatabase: '%s' exists but cannot be opened for reading",
                            m_dbPath.c_str());
            return false;
        }

        const auto reject = [this](size_t lineNumber, const char* reason)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core,
                            "AsyncDatabase: refusing to load '%s': line %zu %s; the file is left untouched",
                            m_dbPath.c_str(), lineNumber, reason);
            return false;
        };

        std::unordered_map<std::string, std::string> loaded;
        std::string line;
        std::string key;
        std::string value;
        bool escapedFormat = false;
        size_t lineNumber = 0;
        while (std::getline(file, line))
        {
            ++lineNumber;
            // Every record the writer emits ends with a newline, so a final line without
            // one is a truncated file.
            if (file.eof())
            {
                return reject(lineNumber, "is not newline-terminated (truncated store)");
            }
#ifdef _WIN32
            // Earlier Windows builds wrote in text mode ("\r\n"). The writer now escapes
            // every CR inside a field, so a trailing CR is only ever that line ending.
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
#endif
            if (lineNumber == 1 && line == kKVFormatMarker)
            {
                escapedFormat = true;
                continue;
            }

            // Files without the marker predate escaping and store raw bytes: they must load
            // verbatim, or a legacy value like "C:\temp" would decode its "\t" into a tab.
            const size_t tabPos = line.find('\t');
            if (tabPos == std::string::npos)
            {
                return reject(lineNumber, "has no key/value separator");
            }
            if (escapedFormat)
            {
                if (!UnescapeKVField(line.substr(0, tabPos), key) || !UnescapeKVField(line.substr(tabPos + 1), value))
                {
                    return reject(lineNumber, "contains an escape sequence the writer never produces");
                }
            }
            else
            {
                key = line.substr(0, tabPos);
                value = line.substr(tabPos + 1);
            }
            if (!loaded.emplace(std::move(key), std::move(value)).second)
            {
                return reject(lineNumber, "repeats a key");
            }
        }

        if (file.bad())
        {
            return reject(lineNumber + 1, "could not be read (I/O error)");
        }

        m_kvStore = std::move(loaded);
        return true;
    }

    // ============================================================================
    // AsyncDatabasePool
    // ============================================================================

    AsyncDatabasePool::~AsyncDatabasePool()
    {
        if (m_open.load())
        {
            Close();
        }
    }

    bool AsyncDatabasePool::Open(const std::string& connectionString, int poolSize)
    {
        if (m_open.load())
        {
            return false;
        }

        m_connectionString = connectionString;
        poolSize = std::max(poolSize, 1);

        // All queries — async (worker threads) and sync — execute through a single
        // shared connection guarded by m_syncMutex (created just below). This is
        // deliberate: the file-based fallback store holds its data in memory, so giving
        // each worker its own connection would give each an independent copy of the same
        // file, cross-connection writes would never be mutually visible, and the last
        // FlushToDisk on shutdown would clobber every other connection's writes. One
        // shared, serialized connection is the single source of truth. Worker threads are
        // still launched below to service the async future/callback API.
        m_syncConnection = std::make_unique<SQLiteConnection>();
        if (!m_syncConnection->Open(connectionString))
        {
            return false;
        }
        for (const auto& [id, sql] : m_preparedSQL)
        {
            m_syncConnection->PrepareStatement(id, sql);
        }

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_stopping.store(false);
            m_accepting = true;
        }
        m_open.store(true);

        // Launch worker threads
        for (int i = 0; i < poolSize; ++i)
        {
            m_workers.emplace_back(&AsyncDatabasePool::WorkerThread, this, i);
        }

        return true;
    }

    void AsyncDatabasePool::Close()
    {
        {
            // Admission and worker exit share the same mutex-protected state.
            // Once m_accepting is false, every item already in m_workQueue is
            // owned by the workers and will be drained before they may exit.
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (!m_open.load() || !m_accepting)
            {
                return;
            }
            m_accepting = false;
            m_stopping.store(true);
        }

        // Wake all workers so they drain accepted work and then stop.
        m_queueCV.notify_all();

        // Join all worker threads
        for (auto& worker : m_workers)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }
        m_workers.clear();

        // Close all connections
        for (auto& conn : m_connections)
        {
            conn->Close();
        }
        m_connections.clear();

        if (m_syncConnection)
        {
            m_syncConnection->Close();
            m_syncConnection.reset();
        }

        m_open.store(false);
    }

    void AsyncDatabasePool::PrepareStatement(PreparedStatementID id, const std::string& sql)
    {
        m_preparedSQL[id] = sql;

        // If already open, also register on all live connections
        for (auto& conn : m_connections)
        {
            conn->PrepareStatement(id, sql);
        }
        if (m_syncConnection)
        {
            m_syncConnection->PrepareStatement(id, sql);
        }
    }

    namespace
    {
        // With the pool closed there are no workers to drain the queue, so enqueueing
        // would strand the item forever (future.get() deadlocks, callbacks never fire).
        // Fail fast with the same error SyncQuery reports.
        QueryResult MakePoolClosedResult()
        {
            QueryResult result;
            result.success = false;
            result.errorMessage = "Database pool is not open";
            return result;
        }
    } // namespace

    std::future<QueryResult> AsyncDatabasePool::AsyncQuery(PreparedStatementID id,
                                                           std::vector<PreparedStatementParam> params)
    {
        WorkItem item;
        item.stmtId = id;
        item.params = std::move(params);
        auto future = item.promise.get_future();

        bool accepted = false;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_accepting)
            {
                m_workQueue.push(std::move(item));
                m_pendingCount.fetch_add(1);
                accepted = true;
            }
        }
        if (accepted)
        {
            m_queueCV.notify_one();
        }
        else
        {
            item.promise.set_value(MakePoolClosedResult());
        }

        return future;
    }

    void AsyncDatabasePool::AsyncQueryWithCallback(PreparedStatementID id, std::vector<PreparedStatementParam> params,
                                                   Spark::Persistence::AsyncQueryCallback callback)
    {
        WorkItem item;
        item.stmtId = id;
        item.params = std::move(params);
        item.callback = std::move(callback);

        bool accepted = false;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_accepting)
            {
                m_workQueue.push(std::move(item));
                m_pendingCount.fetch_add(1);
                accepted = true;
            }
        }
        if (accepted)
        {
            m_queueCV.notify_one();
        }
        else
        {
            // Preserve callback API behavior: closed-pool failures are delivered
            // by ProcessCallbacks() on its calling thread, never inline here.
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            m_completedCallbacks.push_back({std::move(item.callback), MakePoolClosedResult()});
        }
    }

    std::future<QueryResult> AsyncDatabasePool::AsyncTransaction(Transaction transaction)
    {
        WorkItem item;
        item.isTransaction = true;
        item.transaction = std::move(transaction);
        auto future = item.promise.get_future();

        bool accepted = false;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_accepting)
            {
                m_workQueue.push(std::move(item));
                m_pendingCount.fetch_add(1);
                accepted = true;
            }
        }
        if (accepted)
        {
            m_queueCV.notify_one();
        }
        else
        {
            item.promise.set_value(MakePoolClosedResult());
        }

        return future;
    }

    QueryResult AsyncDatabasePool::SyncQuery(PreparedStatementID id, std::vector<PreparedStatementParam> params)
    {
        // Execute on the single shared connection under m_syncMutex — the same connection
        // and mutex the worker threads use — so sync and async queries are fully
        // serialized against one consistent store rather than racing separate stores.
        std::lock_guard<std::mutex> lock(m_syncMutex);
        if (!m_open.load() || !m_syncConnection)
        {
            QueryResult result;
            result.success = false;
            result.errorMessage = "Database pool is not open";
            return result;
        }
        return m_syncConnection->Execute(id, params);
    }

    void AsyncDatabasePool::ProcessCallbacks()
    {
        std::vector<CompletedCallback> local;
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            local.swap(m_completedCallbacks);
        }

        for (auto& completed : local)
        {
            if (completed.callback)
            {
                completed.callback(std::move(completed.result));
            }
        }
    }

    bool AsyncDatabasePool::WaitForWorkItem(WorkItem& item)
    {
        // The queue lock lives for this whole function. The wait predicate
        // guarantees work is queued unless we are stopping, so an empty queue
        // after the wait means "stopping and drained".
        std::unique_lock<std::mutex> lock(m_queueMutex);
        m_queueCV.wait(lock, [this]() { return m_stopping.load() || !m_workQueue.empty(); });
        if (m_workQueue.empty())
        {
            return false;
        }
        item = std::move(m_workQueue.front());
        m_workQueue.pop();
        return true;
    }

    void AsyncDatabasePool::WorkerThread(int threadIndex)
    {
        (void)threadIndex; // All workers share the single serialized m_syncConnection.
        while (true)
        {
            WorkItem item;
            if (!WaitForWorkItem(item))
            {
                return;
            }

            QueryResult result;
            std::exception_ptr queryException;

            try
            {
                // Serialize every database operation through the one shared connection so
                // all workers and SyncQuery see a single consistent in-memory store and
                // file. The lock spans the whole transaction to keep it atomic.
                std::lock_guard<std::mutex> dbLock(m_syncMutex);
                auto& conn = m_syncConnection;
                if (item.isTransaction)
                {
                    bool ok = conn->BeginTransaction();
                    if (!ok)
                    {
                        result.success = false;
                        result.errorMessage = "Failed to begin transaction";
                    }
                    else
                    {
                        bool allSucceeded = true;
                        for (const auto& [stmtId, params] : item.transaction.queries)
                        {
                            QueryResult queryResult = conn->Execute(stmtId, params);
                            if (!queryResult.success)
                            {
                                result = std::move(queryResult);
                                allSucceeded = false;
                                break;
                            }
                            result.affectedRows += queryResult.affectedRows;
                        }

                        if (allSucceeded)
                        {
                            if (conn->CommitTransaction())
                            {
                                result.success = true;
                            }
                            else
                            {
                                result.success = false;
                                result.errorMessage = "Failed to commit transaction";
                            }
                        }
                        else
                        {
                            conn->RollbackTransaction();
                        }
                    }
                }
                else
                {
                    result = conn->Execute(item.stmtId, item.params);
                }
            }
            catch (...)
            {
                queryException = std::current_exception();
            }

            m_pendingCount.fetch_sub(1);

            if (item.callback)
            {
                if (queryException)
                {
                    result = QueryResult{};
                    result.success = false;
                    result.errorMessage = "Query threw exception";
                }
                std::lock_guard<std::mutex> lock(m_callbackMutex);
                m_completedCallbacks.push_back({std::move(item.callback), std::move(result)});
            }
            else
            {
                if (queryException)
                {
                    item.promise.set_exception(queryException);
                }
                else
                {
                    item.promise.set_value(std::move(result));
                }
            }
        }
    }

} // namespace Spark::Persistence
