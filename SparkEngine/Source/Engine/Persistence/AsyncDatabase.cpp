/**
 * @file AsyncDatabase.cpp
 * @brief Implementation of the async database persistence layer.
 */

#include "AsyncDatabase.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

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

        LoadFromDisk();
        m_open = true;
        return true;
    }

    void SQLiteConnection::Close()
    {
        if (!m_open)
        {
            return;
        }

        FlushToDisk();
        m_kvStore.clear();
        m_preparedSQL.clear();
        m_open = false;
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

        // Substitute parameters into the SQL template.
        // Params are referenced as ?0, ?1, ?2... in the prepared SQL string.
        std::string sql = it->second;
        for (size_t i = 0; i < params.size(); ++i)
        {
            std::string placeholder = "?" + std::to_string(i);
            std::string replacement;

            std::visit(
                [&replacement](auto&& arg)
                {
                    using T = std::decay_t<decltype(arg)>;
                    if constexpr (std::is_same_v<T, std::monostate>)
                    {
                        replacement = "NULL";
                    }
                    else if constexpr (std::is_same_v<T, int64_t>)
                    {
                        replacement = std::to_string(arg);
                    }
                    else if constexpr (std::is_same_v<T, double>)
                    {
                        replacement = std::to_string(arg);
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
                        replacement = escaped;
                    }
                    else if constexpr (std::is_same_v<T, std::vector<uint8_t>>)
                    {
                        replacement = "[blob:" + std::to_string(arg.size()) + "]";
                    }
                },
                params[i].value);

            auto pos = sql.find(placeholder);
            if (pos != std::string::npos)
            {
                sql.replace(pos, placeholder.size(), replacement);
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
            std::getline(stream, value);

            // Trim leading whitespace from value
            if (!value.empty() && value[0] == ' ')
            {
                value = value.substr(1);
            }

            m_kvStore[key] = value;
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

            auto erased = m_kvStore.erase(key);
            result.success = true;
            result.affectedRows = static_cast<int>(erased);
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
        m_inTransaction = false;
        m_transactionSnapshot.clear();
        FlushToDisk();
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

    void SQLiteConnection::FlushToDisk()
    {
        std::ofstream file(m_dbPath, std::ios::trunc);
        if (!file.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "AsyncDatabase: failed to open '%s' for writing",
                            m_dbPath.c_str());
            return;
        }

        for (const auto& [key, value] : m_kvStore)
        {
            file << key << '\t' << value << '\n';
        }

        if (file.fail())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "AsyncDatabase: write error flushing '%s' (%zu entries)",
                            m_dbPath.c_str(), m_kvStore.size());
        }
    }

    void SQLiteConnection::LoadFromDisk()
    {
        m_kvStore.clear();

        std::ifstream file(m_dbPath);
        if (!file.is_open())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "AsyncDatabase: '%s' not found — starting with empty store",
                           m_dbPath.c_str());
            return;
        }

        std::string line;
        while (std::getline(file, line))
        {
            auto tabPos = line.find('\t');
            if (tabPos != std::string::npos)
            {
                std::string key = line.substr(0, tabPos);
                std::string value = line.substr(tabPos + 1);
                m_kvStore[key] = value;
            }
        }

        if (file.bad())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "AsyncDatabase: read error loading '%s'", m_dbPath.c_str());
        }
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

        // Create one connection per worker thread
        for (int i = 0; i < poolSize; ++i)
        {
            auto conn = std::make_unique<SQLiteConnection>();
            if (!conn->Open(connectionString))
            {
                return false;
            }

            // Register all prepared statements on this connection
            for (const auto& [id, sql] : m_preparedSQL)
            {
                conn->PrepareStatement(id, sql);
            }

            m_connections.push_back(std::move(conn));
        }

        // Also prepare a sync connection (index 0 is shared with worker 0, guarded by m_syncMutex)
        m_open.store(true);
        m_stopping.store(false);

        // Launch worker threads
        for (int i = 0; i < poolSize; ++i)
        {
            m_workers.emplace_back(&AsyncDatabasePool::WorkerThread, this, i);
        }

        return true;
    }

    void AsyncDatabasePool::Close()
    {
        if (!m_open.load())
        {
            return;
        }

        // Signal workers to stop
        m_stopping.store(true);
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
    }

    std::future<QueryResult> AsyncDatabasePool::AsyncQuery(PreparedStatementID id,
                                                           std::vector<PreparedStatementParam> params)
    {
        WorkItem item;
        item.stmtId = id;
        item.params = std::move(params);
        auto future = item.promise.get_future();

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_workQueue.push(std::move(item));
        }
        m_pendingCount.fetch_add(1);
        m_queueCV.notify_one();

        return future;
    }

    void AsyncDatabasePool::AsyncQueryWithCallback(PreparedStatementID id, std::vector<PreparedStatementParam> params,
                                                   Spark::Persistence::AsyncQueryCallback callback)
    {
        WorkItem item;
        item.stmtId = id;
        item.params = std::move(params);
        item.callback = std::move(callback);

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_workQueue.push(std::move(item));
        }
        m_pendingCount.fetch_add(1);
        m_queueCV.notify_one();
    }

    std::future<QueryResult> AsyncDatabasePool::AsyncTransaction(Transaction transaction)
    {
        WorkItem item;
        item.isTransaction = true;
        item.transaction = std::move(transaction);
        auto future = item.promise.get_future();

        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_workQueue.push(std::move(item));
        }
        m_pendingCount.fetch_add(1);
        m_queueCV.notify_one();

        return future;
    }

    QueryResult AsyncDatabasePool::SyncQuery(PreparedStatementID id, std::vector<PreparedStatementParam> params)
    {
        // Use connection[0] with a dedicated sync mutex to avoid contention with worker 0.
        // In a production setup, a dedicated sync connection would be preferable.
        std::lock_guard<std::mutex> lock(m_syncMutex);
        if (!m_open.load() || m_connections.empty())
        {
            QueryResult result;
            result.success = false;
            result.errorMessage = "Database pool is not open";
            return result;
        }
        return m_connections[0]->Execute(id, params);
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

    void AsyncDatabasePool::WorkerThread(int threadIndex)
    {
        while (true)
        {
            WorkItem item;

            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_queueCV.wait(lock, [this]() { return m_stopping.load() || !m_workQueue.empty(); });

                if (m_stopping.load() && m_workQueue.empty())
                {
                    return;
                }

                item = std::move(m_workQueue.front());
                m_workQueue.pop();
            }

            auto& conn = m_connections[threadIndex];
            QueryResult result;
            std::exception_ptr queryException;

            try
            {
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
                            conn->CommitTransaction();
                            result.success = true;
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
