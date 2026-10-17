// TestAsyncDatabase.cpp - Tests for Spark::Persistence value types (QueryRow, QueryResult,
// PreparedStatementData, Transaction) and a transaction executed through the real pool.
//
// These drive the production types from Engine/Persistence/AsyncDatabase.h. An earlier
// version tested a private reimplementation of the same types, which could never detect a
// regression in the shipped code (SEC2 finding 55).

#include "TestFramework.h"
#include "Engine/Persistence/AsyncDatabase.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace Spark::Persistence;

namespace
{
    QueryRow MakeRow(std::vector<QueryValue> columns)
    {
        QueryRow row;
        row.columns = std::move(columns);
        return row;
    }

    PreparedStatementParam Param(QueryValue value)
    {
        PreparedStatementParam param;
        param.value = std::move(value);
        return param;
    }
} // namespace

// ============================================================================
// QueryResult success / failure
// ============================================================================

TEST(AsyncDatabase_QueryResult_DefaultIsNotSuccess)
{
    const QueryResult result;
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.errorMessage.empty());
}

TEST(AsyncDatabase_QueryResult_Failure)
{
    QueryResult result;
    result.success = false;
    result.errorMessage = "connection refused";
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorMessage, std::string("connection refused"));
}

TEST(AsyncDatabase_QueryResult_FailureHasNoRows)
{
    const QueryResult result;
    EXPECT_FALSE(result.HasRows());
    EXPECT_EQ(result.RowCount(), 0u);
}

// ============================================================================
// QueryRow value access
// ============================================================================

TEST(AsyncDatabase_QueryRow_GetInt)
{
    const QueryRow row = MakeRow({int64_t{42}, 3.14, std::string("hello")});
    EXPECT_EQ(row.GetInt(0), int64_t{42});
}

TEST(AsyncDatabase_QueryRow_GetIntKeepsSixtyFourBits)
{
    const int64_t large = int64_t{1} << 40;
    const QueryRow row = MakeRow({large});
    EXPECT_EQ(row.GetInt(0), large);
}

TEST(AsyncDatabase_QueryRow_GetDouble)
{
    const QueryRow row = MakeRow({int64_t{42}, 3.14, std::string("hello")});
    EXPECT_NEAR(row.GetDouble(1), 3.14, 0.001);
}

TEST(AsyncDatabase_QueryRow_NumericWidening)
{
    const QueryRow row = MakeRow({int64_t{7}, 2.75});
    EXPECT_NEAR(row.GetDouble(0), 7.0, 0.001);
    EXPECT_EQ(row.GetInt(1), int64_t{2});
}

TEST(AsyncDatabase_QueryRow_GetString)
{
    const QueryRow row = MakeRow({int64_t{42}, 3.14, std::string("hello")});
    EXPECT_EQ(row.GetString(2), std::string("hello"));
}

TEST(AsyncDatabase_QueryRow_IsNull)
{
    const QueryRow row = MakeRow({std::monostate{}, int64_t{10}});
    EXPECT_TRUE(row.IsNull(0));
    EXPECT_FALSE(row.IsNull(1));
}

TEST(AsyncDatabase_QueryRow_IsNull_OutOfBounds)
{
    const QueryRow row = MakeRow({int64_t{1}});
    EXPECT_TRUE(row.IsNull(99));
}

TEST(AsyncDatabase_QueryRow_TypeMismatchReturnsDefault)
{
    const QueryRow row = MakeRow({std::string("not a number"), int64_t{5}});
    EXPECT_EQ(row.GetInt(0), int64_t{0});
    EXPECT_NEAR(row.GetDouble(0), 0.0, 0.001);
    EXPECT_TRUE(row.GetString(1).empty());
}

TEST(AsyncDatabase_QueryRow_OutOfRangeReturnsDefault)
{
    const QueryRow row = MakeRow({int64_t{1}});
    EXPECT_EQ(row.GetInt(3), int64_t{0});
    EXPECT_NEAR(row.GetDouble(3), 0.0, 0.001);
    EXPECT_TRUE(row.GetString(3).empty());
}

// ============================================================================
// HasRows / RowCount / affected rows
// ============================================================================

TEST(AsyncDatabase_QueryResult_HasRows)
{
    QueryResult result;
    result.success = true;
    result.rows.push_back(MakeRow({int64_t{1}}));
    result.rows.push_back(MakeRow({int64_t{2}}));

    EXPECT_TRUE(result.HasRows());
    EXPECT_EQ(result.RowCount(), 2u);
}

TEST(AsyncDatabase_QueryResult_AffectedRowsAndLastInsertIdDefaultZero)
{
    const QueryResult result;
    EXPECT_EQ(result.affectedRows, 0);
    EXPECT_EQ(result.lastInsertId, int64_t{0});
}

// ============================================================================
// PreparedStatementData set / clear params
// ============================================================================

TEST(AsyncDatabase_PreparedStatement_SetParams)
{
    PreparedStatementData stmt;
    stmt.SetInt(0, 10);
    stmt.SetDouble(1, 2.5);
    stmt.SetString(2, "test");

    ASSERT_EQ(stmt.params.size(), 3u);
    EXPECT_EQ(std::get<int64_t>(stmt.params[0].value), int64_t{10});
    EXPECT_NEAR(std::get<double>(stmt.params[1].value), 2.5, 0.001);
    EXPECT_EQ(std::get<std::string>(stmt.params[2].value), std::string("test"));
}

TEST(AsyncDatabase_PreparedStatement_SetNull)
{
    PreparedStatementData stmt;
    stmt.SetInt(0, 3);
    stmt.SetNull(0);
    ASSERT_EQ(stmt.params.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(stmt.params[0].value));
}

TEST(AsyncDatabase_PreparedStatement_ClearParams)
{
    PreparedStatementData stmt;
    stmt.SetInt(0, 42);
    EXPECT_EQ(stmt.params.size(), 1u);

    stmt.ClearParams();
    EXPECT_TRUE(stmt.params.empty());
}

TEST(AsyncDatabase_PreparedStatement_SparseIndex)
{
    PreparedStatementData stmt;
    stmt.SetInt(3, 99);

    // Indices 0-2 are NULL (monostate); index 3 holds the value.
    ASSERT_EQ(stmt.params.size(), 4u);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(stmt.params[0].value));
    EXPECT_EQ(std::get<int64_t>(stmt.params[3].value), int64_t{99});
}

TEST(AsyncDatabase_PreparedStatement_OverwriteKeepsCount)
{
    PreparedStatementData stmt;
    stmt.SetString(1, "first");
    stmt.SetString(1, "second");
    ASSERT_EQ(stmt.params.size(), 2u);
    EXPECT_EQ(std::get<std::string>(stmt.params[1].value), std::string("second"));
}

// ============================================================================
// Transaction append and size
// ============================================================================

TEST(AsyncDatabase_Transaction_Empty)
{
    const Transaction tx;
    EXPECT_EQ(tx.Size(), 0u);
    EXPECT_TRUE(tx.queries.empty());
}

TEST(AsyncDatabase_Transaction_Append)
{
    Transaction tx;
    tx.Append(1, {Param(std::string("Alice"))});
    tx.Append(1, {Param(std::string("Bob"))});
    EXPECT_EQ(tx.Size(), 2u);
}

TEST(AsyncDatabase_Transaction_KeepsStatementIdsAndParams)
{
    Transaction tx;
    tx.Append(7);
    tx.Append(9, {Param(int64_t{5}), Param(std::monostate{})});

    ASSERT_EQ(tx.Size(), 2u);
    EXPECT_EQ(tx.queries[0].first, PreparedStatementID{7});
    EXPECT_TRUE(tx.queries[0].second.empty());
    EXPECT_EQ(tx.queries[1].first, PreparedStatementID{9});
    ASSERT_EQ(tx.queries[1].second.size(), 2u);
    EXPECT_EQ(std::get<int64_t>(tx.queries[1].second[0].value), int64_t{5});
}

TEST(AsyncDatabase_Transaction_CommitsThroughRealPool)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "spark_asyncdb_transaction.kv";
    std::filesystem::remove(path);

    constexpr PreparedStatementID kSetName = 1;
    constexpr PreparedStatementID kSetLevel = 2;
    constexpr PreparedStatementID kGetName = 3;
    AsyncDatabasePool pool;
    pool.PrepareStatement(kSetName, "SET hero.name ?0");
    pool.PrepareStatement(kSetLevel, "SET hero.level ?0");
    pool.PrepareStatement(kGetName, "GET hero.name");
    ASSERT_TRUE(pool.Open(path.string(), 1));

    Transaction tx;
    tx.Append(kSetName, {Param(std::string("Aria"))});
    tx.Append(kSetLevel, {Param(int64_t{12})});
    const QueryResult committed = pool.AsyncTransaction(std::move(tx)).get();
    EXPECT_TRUE(committed.success);

    const QueryResult name = pool.SyncQuery(kGetName);
    ASSERT_TRUE(name.HasRows());
    EXPECT_EQ(name.rows[0].GetString(0), std::string("Aria"));
    pool.Close();

    std::filesystem::remove(path);
    std::filesystem::path lock = path;
    lock += ".lock";
    std::filesystem::remove(lock);
}
