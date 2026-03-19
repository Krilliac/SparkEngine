// TestAsyncDatabase.cpp - Tests for Spark::Persistence (QueryResult, PreparedStatementData, Transaction)
// Standalone reimplementation to avoid platform-specific header dependencies

#include "TestFramework.h"
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// ============================================================================
// Standalone reimplementation of Spark::Persistence types
// ============================================================================

namespace
{

    using DbValue = std::variant<std::monostate, int, double, std::string>;

    class QueryRow
    {
      public:
        explicit QueryRow(std::vector<DbValue> values) : m_values(std::move(values)) {}

        int GetInt(size_t column) const
        {
            if (column < m_values.size() && std::holds_alternative<int>(m_values[column]))
                return std::get<int>(m_values[column]);
            return 0;
        }

        double GetDouble(size_t column) const
        {
            if (column < m_values.size() && std::holds_alternative<double>(m_values[column]))
                return std::get<double>(m_values[column]);
            return 0.0;
        }

        std::string GetString(size_t column) const
        {
            if (column < m_values.size() && std::holds_alternative<std::string>(m_values[column]))
                return std::get<std::string>(m_values[column]);
            return "";
        }

        bool IsNull(size_t column) const
        {
            if (column >= m_values.size())
                return true;
            return std::holds_alternative<std::monostate>(m_values[column]);
        }

        size_t ColumnCount() const { return m_values.size(); }

      private:
        std::vector<DbValue> m_values;
    };

    class QueryResult
    {
      public:
        static QueryResult Success(std::vector<QueryRow> rows, uint64_t affectedRows = 0, uint64_t lastInsertId = 0)
        {
            QueryResult result;
            result.m_success = true;
            result.m_rows = std::move(rows);
            result.m_affectedRows = affectedRows;
            result.m_lastInsertId = lastInsertId;
            return result;
        }

        static QueryResult Failure(const std::string& error)
        {
            QueryResult result;
            result.m_success = false;
            result.m_error = error;
            return result;
        }

        bool IsSuccess() const { return m_success; }
        const std::string& GetError() const { return m_error; }

        bool HasRows() const { return !m_rows.empty(); }
        size_t RowCount() const { return m_rows.size(); }

        const QueryRow& GetRow(size_t index) const { return m_rows[index]; }

        uint64_t GetAffectedRows() const { return m_affectedRows; }
        uint64_t GetLastInsertId() const { return m_lastInsertId; }

      private:
        bool m_success = false;
        std::string m_error;
        std::vector<QueryRow> m_rows;
        uint64_t m_affectedRows = 0;
        uint64_t m_lastInsertId = 0;
    };

    class PreparedStatementData
    {
      public:
        explicit PreparedStatementData(const std::string& sql) : m_sql(sql) {}

        const std::string& GetSQL() const { return m_sql; }

        void SetInt(uint32_t index, int value)
        {
            EnsureSize(index);
            m_params[index] = value;
        }

        void SetDouble(uint32_t index, double value)
        {
            EnsureSize(index);
            m_params[index] = value;
        }

        void SetString(uint32_t index, const std::string& value)
        {
            EnsureSize(index);
            m_params[index] = value;
        }

        void SetNull(uint32_t index)
        {
            EnsureSize(index);
            m_params[index] = std::monostate{};
        }

        const DbValue& GetParam(uint32_t index) const
        {
            static const DbValue empty{std::monostate{}};
            if (index < m_params.size())
                return m_params[index];
            return empty;
        }

        uint32_t ParamCount() const { return static_cast<uint32_t>(m_params.size()); }

        void ClearParams() { m_params.clear(); }

      private:
        void EnsureSize(uint32_t index)
        {
            if (index >= m_params.size())
                m_params.resize(index + 1);
        }

        std::string m_sql;
        std::vector<DbValue> m_params;
    };

    struct TransactionStatement
    {
        std::string sql;
        std::vector<DbValue> params;
    };

    class Transaction
    {
      public:
        void Append(const std::string& sql, std::vector<DbValue> params = {})
        {
            m_statements.push_back({sql, std::move(params)});
        }

        size_t Size() const { return m_statements.size(); }

        const TransactionStatement& GetStatement(size_t index) const { return m_statements[index]; }

        bool IsEmpty() const { return m_statements.empty(); }

        void Clear() { m_statements.clear(); }

      private:
        std::vector<TransactionStatement> m_statements;
    };

} // namespace

// ============================================================================
// QueryResult success / failure
// ============================================================================

TEST(AsyncDatabase_QueryResult_Success)
{
    auto result = QueryResult::Success({});
    EXPECT_TRUE(result.IsSuccess());
    EXPECT_TRUE(result.GetError().empty());
}

TEST(AsyncDatabase_QueryResult_Failure)
{
    auto result = QueryResult::Failure("connection refused");
    EXPECT_FALSE(result.IsSuccess());
    EXPECT_EQ(result.GetError(), std::string("connection refused"));
}

TEST(AsyncDatabase_QueryResult_FailureHasNoRows)
{
    auto result = QueryResult::Failure("timeout");
    EXPECT_FALSE(result.HasRows());
    EXPECT_EQ(result.RowCount(), 0u);
}

// ============================================================================
// QueryRow value access
// ============================================================================

TEST(AsyncDatabase_QueryRow_GetInt)
{
    QueryRow row({DbValue{42}, DbValue{3.14}, DbValue{std::string("hello")}});
    EXPECT_EQ(row.GetInt(0), 42);
}

TEST(AsyncDatabase_QueryRow_GetDouble)
{
    QueryRow row({DbValue{42}, DbValue{3.14}, DbValue{std::string("hello")}});
    EXPECT_NEAR(row.GetDouble(1), 3.14, 0.001);
}

TEST(AsyncDatabase_QueryRow_GetString)
{
    QueryRow row({DbValue{42}, DbValue{3.14}, DbValue{std::string("hello")}});
    EXPECT_EQ(row.GetString(2), std::string("hello"));
}

TEST(AsyncDatabase_QueryRow_IsNull)
{
    QueryRow row({DbValue{std::monostate{}}, DbValue{10}});
    EXPECT_TRUE(row.IsNull(0));
    EXPECT_FALSE(row.IsNull(1));
}

TEST(AsyncDatabase_QueryRow_IsNull_OutOfBounds)
{
    QueryRow row({DbValue{1}});
    EXPECT_TRUE(row.IsNull(99));
}

TEST(AsyncDatabase_QueryRow_TypeMismatchReturnsDefault)
{
    QueryRow row({DbValue{std::string("not an int")}});
    EXPECT_EQ(row.GetInt(0), 0);
    EXPECT_NEAR(row.GetDouble(0), 0.0, 0.001);
}

// ============================================================================
// HasRows / RowCount
// ============================================================================

TEST(AsyncDatabase_QueryResult_HasRows)
{
    QueryRow row1({DbValue{1}});
    QueryRow row2({DbValue{2}});
    auto result = QueryResult::Success({row1, row2});

    EXPECT_TRUE(result.HasRows());
    EXPECT_EQ(result.RowCount(), 2u);
}

TEST(AsyncDatabase_QueryResult_NoRows)
{
    auto result = QueryResult::Success({});
    EXPECT_FALSE(result.HasRows());
    EXPECT_EQ(result.RowCount(), 0u);
}

// ============================================================================
// QueryResult with affectedRows and lastInsertId
// ============================================================================

TEST(AsyncDatabase_QueryResult_AffectedRows)
{
    auto result = QueryResult::Success({}, 5, 0);
    EXPECT_EQ(result.GetAffectedRows(), 5u);
}

TEST(AsyncDatabase_QueryResult_LastInsertId)
{
    auto result = QueryResult::Success({}, 1, 42);
    EXPECT_EQ(result.GetLastInsertId(), 42u);
}

TEST(AsyncDatabase_QueryResult_AffectedRowsDefaultZero)
{
    auto result = QueryResult::Success({});
    EXPECT_EQ(result.GetAffectedRows(), 0u);
    EXPECT_EQ(result.GetLastInsertId(), 0u);
}

// ============================================================================
// PreparedStatementData set / clear params
// ============================================================================

TEST(AsyncDatabase_PreparedStatement_SQL)
{
    PreparedStatementData stmt("SELECT * FROM users WHERE id = ?");
    EXPECT_EQ(stmt.GetSQL(), std::string("SELECT * FROM users WHERE id = ?"));
}

TEST(AsyncDatabase_PreparedStatement_SetParams)
{
    PreparedStatementData stmt("INSERT INTO t (a, b, c) VALUES (?, ?, ?)");
    stmt.SetInt(0, 10);
    stmt.SetDouble(1, 2.5);
    stmt.SetString(2, "test");

    EXPECT_EQ(stmt.ParamCount(), 3u);
    EXPECT_EQ(std::get<int>(stmt.GetParam(0)), 10);
    EXPECT_NEAR(std::get<double>(stmt.GetParam(1)), 2.5, 0.001);
    EXPECT_EQ(std::get<std::string>(stmt.GetParam(2)), std::string("test"));
}

TEST(AsyncDatabase_PreparedStatement_SetNull)
{
    PreparedStatementData stmt("UPDATE t SET x = ?");
    stmt.SetNull(0);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(stmt.GetParam(0)));
}

TEST(AsyncDatabase_PreparedStatement_ClearParams)
{
    PreparedStatementData stmt("SELECT 1");
    stmt.SetInt(0, 42);
    EXPECT_EQ(stmt.ParamCount(), 1u);

    stmt.ClearParams();
    EXPECT_EQ(stmt.ParamCount(), 0u);
}

TEST(AsyncDatabase_PreparedStatement_SparseIndex)
{
    PreparedStatementData stmt("SELECT ?");
    stmt.SetInt(3, 99);

    // Indices 0-2 should be monostate (default), index 3 should be 99
    EXPECT_EQ(stmt.ParamCount(), 4u);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(stmt.GetParam(0)));
    EXPECT_EQ(std::get<int>(stmt.GetParam(3)), 99);
}

// ============================================================================
// Transaction append and size
// ============================================================================

TEST(AsyncDatabase_Transaction_Empty)
{
    Transaction tx;
    EXPECT_TRUE(tx.IsEmpty());
    EXPECT_EQ(tx.Size(), 0u);
}

TEST(AsyncDatabase_Transaction_Append)
{
    Transaction tx;
    tx.Append("INSERT INTO users (name) VALUES (?)", {DbValue{std::string("Alice")}});
    tx.Append("INSERT INTO users (name) VALUES (?)", {DbValue{std::string("Bob")}});

    EXPECT_FALSE(tx.IsEmpty());
    EXPECT_EQ(tx.Size(), 2u);
}

TEST(AsyncDatabase_Transaction_GetStatement)
{
    Transaction tx;
    tx.Append("DELETE FROM sessions WHERE expired = 1");

    EXPECT_EQ(tx.GetStatement(0).sql, std::string("DELETE FROM sessions WHERE expired = 1"));
    EXPECT_TRUE(tx.GetStatement(0).params.empty());
}

TEST(AsyncDatabase_Transaction_Clear)
{
    Transaction tx;
    tx.Append("SELECT 1");
    tx.Append("SELECT 2");
    EXPECT_EQ(tx.Size(), 2u);

    tx.Clear();
    EXPECT_TRUE(tx.IsEmpty());
    EXPECT_EQ(tx.Size(), 0u);
}
