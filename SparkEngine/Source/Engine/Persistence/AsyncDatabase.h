/**
 * @file AsyncDatabase.h
 * @brief Async database layer with connection pooling and prepared statements (TC-inspired)
 *
 * TrinityCore-inspired patterns:
 * - DatabaseWorkerPool -> AsyncDatabasePool (thread pool + connection pool)
 * - PreparedStatement -> PreparedQuery (pre-compiled queries referenced by ID)
 * - Async queries via std::future
 * - Transaction batching for atomic multi-query operations
 *
 * Uses SQLite by default (zero-config, file-based). Interface is abstract
 * enough to support MySQL/PostgreSQL backends via IDatabaseConnection.
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

namespace Spark::Persistence
{

    // ============================================================================
    // Query Result Types
    // ============================================================================

    /// @brief A single value returned from a query column.
    /// monostate represents SQL NULL.
    using QueryValue = std::variant<std::monostate, int64_t, double, std::string, std::vector<uint8_t>>;

    /// @brief A single row of query results.
    struct QueryRow
    {
        std::vector<QueryValue> columns;

        /// @brief Get a column as int64. Throws std::bad_variant_access if wrong type.
        int64_t GetInt(size_t col) const;

        /// @brief Get a column as double. Throws std::bad_variant_access if wrong type.
        double GetDouble(size_t col) const;

        /// @brief Get a column as string reference. Throws std::bad_variant_access if wrong type.
        const std::string& GetString(size_t col) const;

        /// @brief Check whether a column holds NULL (monostate).
        bool IsNull(size_t col) const;
    };

    /// @brief Result of a database query or transaction.
    struct QueryResult
    {
        bool success = false;       ///< True if the query executed without error.
        std::string errorMessage;   ///< Error description (empty on success).
        std::vector<QueryRow> rows; ///< Result rows (empty for INSERT/UPDATE/DELETE).
        int affectedRows = 0;       ///< Number of rows modified by the query.
        int64_t lastInsertId = 0;   ///< Auto-incremented ID from the last INSERT.

        bool HasRows() const { return !rows.empty(); }
        size_t RowCount() const { return rows.size(); }
    };

    // ============================================================================
    // Prepared Statement
    // ============================================================================

    using PreparedStatementID = uint32_t;

    /// @brief A single parameter bound to a prepared statement slot.
    struct PreparedStatementParam
    {
        QueryValue value;
    };

    /// @brief Prepared statement handle with bindable parameters.
    struct PreparedStatementData
    {
        PreparedStatementID id = 0;
        std::vector<PreparedStatementParam> params;

        void SetInt(size_t index, int64_t value);
        void SetDouble(size_t index, double value);
        void SetString(size_t index, const std::string& value);
        void SetNull(size_t index);
        void ClearParams();
    };

    // ============================================================================
    // Transaction
    // ============================================================================

    /// @brief A batch of queries executed atomically (all-or-nothing).
    struct Transaction
    {
        std::vector<std::pair<PreparedStatementID, std::vector<PreparedStatementParam>>> queries;

        void Append(PreparedStatementID stmtId, std::vector<PreparedStatementParam> params = {});
        size_t Size() const { return queries.size(); }
    };

    // ============================================================================
    // Database Connection Interface
    // ============================================================================

    /// @brief Abstract interface for a single database connection.
    /// Implement for SQLite, MySQL, PostgreSQL, etc.
    class IDatabaseConnection
    {
      public:
        virtual ~IDatabaseConnection() = default;

        virtual bool Open(const std::string& connectionString) = 0;
        virtual void Close() = 0;
        virtual bool IsOpen() const = 0;

        virtual bool PrepareStatement(PreparedStatementID id, const std::string& sql) = 0;
        virtual QueryResult Execute(PreparedStatementID id, const std::vector<PreparedStatementParam>& params) = 0;
        virtual QueryResult ExecuteRaw(const std::string& sql) = 0;

        virtual bool BeginTransaction() = 0;
        virtual bool CommitTransaction() = 0;
        virtual bool RollbackTransaction() = 0;
    };

    // ============================================================================
    // SQLite Connection (file-based fallback implementation)
    // ============================================================================

    /**
 * @brief File-based key-value store implementing IDatabaseConnection.
 *
 * This is a fallback for when no real SQLite library is linked. It stores
 * data as a JSON key-value file on disk. The interface is identical so that
 * a real SQLite (or MySQL) backend can be swapped in without changing callers.
 */
    class SQLiteConnection : public IDatabaseConnection
    {
      public:
        ~SQLiteConnection() override;

        bool Open(const std::string& connectionString) override;
        void Close() override;
        bool IsOpen() const override { return m_open; }

        bool PrepareStatement(PreparedStatementID id, const std::string& sql) override;
        QueryResult Execute(PreparedStatementID id, const std::vector<PreparedStatementParam>& params) override;
        QueryResult ExecuteRaw(const std::string& sql) override;

        bool BeginTransaction() override;
        bool CommitTransaction() override;
        bool RollbackTransaction() override;

      private:
        void LoadFromDisk();
        void FlushToDisk();

        bool m_open = false;
        bool m_inTransaction = false;
        std::string m_dbPath;
        std::unordered_map<PreparedStatementID, std::string> m_preparedSQL;
        std::unordered_map<std::string, std::string> m_kvStore;
        std::unordered_map<std::string, std::string> m_transactionSnapshot;
    };

    // ============================================================================
    // Async Database Pool (TC's DatabaseWorkerPool equivalent)
    // ============================================================================

    using AsyncQueryCallback = std::function<void(QueryResult)>;

    /**
 * @brief Thread-pool based async database with connection pooling.
 *
 * Each worker thread owns a dedicated database connection. Queries are
 * enqueued from any thread and executed asynchronously. Results are
 * delivered via std::future or callback.
 *
 * Call ProcessCallbacks() each frame on the main thread if using the
 * callback-based API, so callbacks run on the main thread.
 */
    class AsyncDatabasePool
    {
      public:
        AsyncDatabasePool() = default;
        ~AsyncDatabasePool();

        // Non-copyable, non-movable
        AsyncDatabasePool(const AsyncDatabasePool&) = delete;
        AsyncDatabasePool& operator=(const AsyncDatabasePool&) = delete;

        /**
     * @brief Open the pool with the given connection string and thread count.
     * @param connectionString Database file path (SQLite) or connection URL.
     * @param poolSize Number of worker threads and connections to create.
     * @return True if all connections opened successfully.
     */
        bool Open(const std::string& connectionString, int poolSize = 2);

        /// @brief Shut down the pool, joining all worker threads.
        void Close();

        /**
     * @brief Register a prepared statement SQL string by ID.
     * Call during initialization, before issuing queries.
     */
        void PrepareStatement(PreparedStatementID id, const std::string& sql);

        /// @brief Enqueue an async query, returning a future for the result.
        std::future<QueryResult> AsyncQuery(PreparedStatementID id, std::vector<PreparedStatementParam> params = {});

        /// @brief Enqueue an async query with a completion callback.
        /// The callback is dispatched on the main thread via ProcessCallbacks().
        void AsyncQueryWithCallback(PreparedStatementID id, std::vector<PreparedStatementParam> params,
                                    Spark::Persistence::AsyncQueryCallback callback);

        /// @brief Execute a transaction (multiple queries) atomically and asynchronously.
        std::future<QueryResult> AsyncTransaction(Transaction transaction);

        /**
     * @brief Execute a query synchronously on the calling thread.
     * Uses a dedicated synchronous connection. Blocks the caller.
     */
        QueryResult SyncQuery(PreparedStatementID id, std::vector<PreparedStatementParam> params = {});

        /**
     * @brief Dispatch completed async callbacks on the calling (main) thread.
     * Call once per frame from the main loop.
     */
        void ProcessCallbacks();

        bool IsOpen() const { return m_open.load(); }
        int GetPendingQueryCount() const { return m_pendingCount.load(); }
        int GetPoolSize() const { return static_cast<int>(m_workers.size()); }

      private:
        struct WorkItem
        {
            PreparedStatementID stmtId = 0;
            std::vector<PreparedStatementParam> params;
            std::promise<QueryResult> promise;
            Spark::Persistence::AsyncQueryCallback callback;
            bool isTransaction = false;
            Transaction transaction;
        };

        struct CompletedCallback
        {
            Spark::Persistence::AsyncQueryCallback callback;
            QueryResult result;
        };

        void WorkerThread(int threadIndex);

        std::string m_connectionString;
        std::unordered_map<PreparedStatementID, std::string> m_preparedSQL;

        // Worker threads, each owning one connection
        std::vector<std::thread> m_workers;
        std::vector<std::unique_ptr<IDatabaseConnection>> m_connections;

        // Synchronous connection (used by SyncQuery on the main thread)
        std::unique_ptr<IDatabaseConnection> m_syncConnection;
        std::mutex m_syncMutex;

        // Work queue
        std::queue<WorkItem> m_workQueue;
        std::mutex m_queueMutex;
        std::condition_variable m_queueCV;

        // Completed callbacks (dispatched on main thread)
        std::vector<CompletedCallback> m_completedCallbacks;
        std::mutex m_callbackMutex;

        std::atomic<bool> m_open{false};
        std::atomic<bool> m_stopping{false};
        std::atomic<int> m_pendingCount{0};
    };

} // namespace Spark::Persistence
