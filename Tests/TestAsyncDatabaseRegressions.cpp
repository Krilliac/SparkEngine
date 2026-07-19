// TestAsyncDatabaseRegressions.cpp - Regression tests for two AsyncDatabase.cpp bugfixes
// using the real Spark::Persistence::SQLiteConnection directly (synchronous, single-threaded,
// no worker-thread machinery involved — cheap, pure-logic coverage per the fix brief):
//   1. ?N prepared-statement placeholder substitution must not corrupt ?1 when the SQL also
//      contains ?10/?11 (substring-prefix collision).
//   2. ExecuteRaw's SET handler must not truncate values containing embedded newlines.

#include "TestFramework.h"
#include "Engine/Persistence/AsyncDatabase.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace Spark::Persistence;

namespace
{
    // Each test gets its own throwaway db file so tests don't interfere with each other.
    std::string MakeTempDbPath(const char* name)
    {
        auto path = std::filesystem::temp_directory_path() / (std::string("spark_asyncdb_regress_") + name + ".kv");
        std::filesystem::remove(path);
        return path.string();
    }
} // namespace

TEST(AsyncDatabase_PlaceholderSubstitution_DoesNotCorrupt10Plus)
{
    SQLiteConnection conn;
    EXPECT_TRUE(conn.Open(MakeTempDbPath("placeholders")));

    // 12 placeholders (?0..?11) so ?1 is a prefix of ?10 and ?11.
    const PreparedStatementID stmtId = 1;
    EXPECT_TRUE(conn.PrepareStatement(stmtId, "SET result ?0 ?1 ?2 ?3 ?4 ?5 ?6 ?7 ?8 ?9 ?10 ?11"));

    std::vector<PreparedStatementParam> params;
    for (int64_t i = 100; i <= 111; ++i)
    {
        PreparedStatementParam p;
        p.value = i;
        params.push_back(p);
    }

    auto result = conn.Execute(stmtId, params);
    EXPECT_TRUE(result.success);

    auto getResult = conn.ExecuteRaw("GET result");
    EXPECT_TRUE(getResult.success);
    EXPECT_TRUE(getResult.HasRows());

    if (getResult.HasRows())
    {
        EXPECT_EQ(getResult.rows[0].GetString(0), std::string("100 101 102 103 104 105 106 107 108 109 110 111"));
    }

    conn.Close();
}

TEST(AsyncDatabase_SetValue_EmbeddedNewlineNotTruncated)
{
    SQLiteConnection conn;
    EXPECT_TRUE(conn.Open(MakeTempDbPath("newlines")));

    auto setResult = conn.ExecuteRaw("SET mykey line1\nline2\nline3");
    EXPECT_TRUE(setResult.success);

    auto getResult = conn.ExecuteRaw("GET mykey");
    EXPECT_TRUE(getResult.success);
    EXPECT_TRUE(getResult.HasRows());

    if (getResult.HasRows())
    {
        EXPECT_EQ(getResult.rows[0].GetString(0), std::string("line1\nline2\nline3"));
    }

    conn.Close();
}

TEST(AsyncDatabase_LegacyKVFile_LoadsRawBackslashes)
{
    // Files written before the escaped KV format have no format-marker header
    // and store raw bytes. Loading one must not run the unescaper: a legacy
    // value like "C:\temp\a.txt" would otherwise decode "\t" into a tab.
    const std::string dbPath = MakeTempDbPath("legacy");
    {
        std::ofstream legacy(dbPath, std::ios::trunc);
        legacy << "winpath\tC:\\temp\\a.txt\n";
        legacy << "slashes\tfoo\\bar\n";
    }

    SQLiteConnection conn;
    EXPECT_TRUE(conn.Open(dbPath));

    auto pathResult = conn.ExecuteRaw("GET winpath");
    EXPECT_TRUE(pathResult.HasRows());
    if (pathResult.HasRows())
    {
        EXPECT_EQ(pathResult.rows[0].GetString(0), std::string("C:\\temp\\a.txt"));
    }

    auto slashResult = conn.ExecuteRaw("GET slashes");
    EXPECT_TRUE(slashResult.HasRows());
    if (slashResult.HasRows())
    {
        EXPECT_EQ(slashResult.rows[0].GetString(0), std::string("foo\\bar"));
    }

    // A flush rewrites the file in the marked, escaped format; values must
    // round-trip unchanged through the upgrade.
    conn.Close();
    SQLiteConnection reopened;
    EXPECT_TRUE(reopened.Open(dbPath));
    auto upgraded = reopened.ExecuteRaw("GET winpath");
    EXPECT_TRUE(upgraded.HasRows());
    if (upgraded.HasRows())
    {
        EXPECT_EQ(upgraded.rows[0].GetString(0), std::string("C:\\temp\\a.txt"));
    }
    reopened.Close();
}
