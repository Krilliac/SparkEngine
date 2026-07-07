// Test_persistence_AsyncDatabaseParams.cpp
// Regression for the P1 prepared-statement substitution bug in SQLiteConnection::Execute.
// Two defects the old sequential in-place substitution had:
//   1. A placeholder used more than once was only replaced at its FIRST occurrence
//      (the loop `break`-ed after one match).
//   2. Substituting a later placeholder re-scanned already-inserted text, so a string
//      parameter whose value literally contained "?N" got corrupted when ?N was bound.
// The fix does a single left-to-right pass emitting into a fresh output string.

#include "TestFramework.h"
#include "Engine/Persistence/AsyncDatabase.h"

#include <filesystem>
#include <string>
#include <vector>

using namespace Spark::Persistence;

namespace
{
    std::string MakeTempDbPath(const char* name)
    {
        auto path = std::filesystem::temp_directory_path() / (std::string("spark_harden_params_") + name + ".kv");
        std::filesystem::remove(path);
        return path.string();
    }
} // namespace

TEST(AsyncDatabaseParams_DuplicatePlaceholder_ReplacedEveryOccurrence)
{
    SQLiteConnection conn;
    EXPECT_TRUE(conn.Open(MakeTempDbPath("dup")));

    const PreparedStatementID stmtId = 1;
    EXPECT_TRUE(conn.PrepareStatement(stmtId, "SET dup ?0 and ?0 again"));

    std::vector<PreparedStatementParam> params;
    PreparedStatementParam p;
    p.value = static_cast<int64_t>(5);
    params.push_back(p);

    EXPECT_TRUE(conn.Execute(stmtId, params).success);

    auto get = conn.ExecuteRaw("GET dup");
    EXPECT_TRUE(get.HasRows());
    if (get.HasRows())
    {
        // Both occurrences of ?0 must be substituted, not just the first.
        EXPECT_EQ(get.rows[0].GetString(0), std::string("5 and 5 again"));
    }

    conn.Close();
}

TEST(AsyncDatabaseParams_ValueContainingPlaceholderText_NotCorrupted)
{
    SQLiteConnection conn;
    EXPECT_TRUE(conn.Open(MakeTempDbPath("collide")));

    const PreparedStatementID stmtId = 1;
    EXPECT_TRUE(conn.PrepareStatement(stmtId, "SET vp ?0 ?1"));

    // Param 0 is a string whose value literally contains "?1". The old code would,
    // when binding ?1, find that "?1" inside the already-inserted value and clobber it.
    std::vector<PreparedStatementParam> params;
    PreparedStatementParam p0;
    p0.value = std::string("?1");
    PreparedStatementParam p1;
    p1.value = std::string("X");
    params.push_back(p0);
    params.push_back(p1);

    EXPECT_TRUE(conn.Execute(stmtId, params).success);

    auto get = conn.ExecuteRaw("GET vp");
    EXPECT_TRUE(get.HasRows());
    if (get.HasRows())
    {
        // ?0 -> '?1' (quoted/escaped), ?1 -> 'X'. The inserted "'?1'" must survive intact.
        EXPECT_EQ(get.rows[0].GetString(0), std::string("'?1' 'X'"));
    }

    conn.Close();
}
