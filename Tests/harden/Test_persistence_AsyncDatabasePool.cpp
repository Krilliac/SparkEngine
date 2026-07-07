// Test_persistence_AsyncDatabasePool.cpp
// Regression for the P0 AsyncDatabasePool data-loss bug: worker (async) and sync
// queries used to run against independent per-connection in-memory stores, so
//   (a) async-written data was not visible to a subsequent SyncQuery, and
//   (b) on Close the sync connection's flush clobbered every async write, and
//   (c) plain (non-transactional) writes never persisted until Close.
// After the fix all queries share one serialized connection and non-transactional
// writes flush immediately, so writes are cross-visible in-session AND survive a
// full close/reopen cycle.

#include "TestFramework.h"
#include "Engine/Persistence/AsyncDatabase.h"

#include <filesystem>
#include <string>

using namespace Spark::Persistence;

namespace
{
    std::string MakeTempDbPath(const char* name)
    {
        auto path = std::filesystem::temp_directory_path() / (std::string("spark_harden_pool_") + name + ".kv");
        std::filesystem::remove(path);
        return path.string();
    }

    // Prepared-statement IDs used by the tests below.
    constexpr PreparedStatementID kSetAsync = 1;
    constexpr PreparedStatementID kSetSync = 2;
    constexpr PreparedStatementID kGetAsync = 3;
    constexpr PreparedStatementID kGetSync = 4;
} // namespace

TEST(AsyncDatabasePool_AsyncWrite_VisibleToSyncQuery_SameSession)
{
    const std::string path = MakeTempDbPath("visibility");

    AsyncDatabasePool pool;
    pool.PrepareStatement(kSetAsync, "SET akey aval");
    pool.PrepareStatement(kGetAsync, "GET akey");
    EXPECT_TRUE(pool.Open(path, 2));

    // Issue the write on a worker thread and wait for it to complete.
    auto future = pool.AsyncQuery(kSetAsync);
    QueryResult writeResult = future.get();
    EXPECT_TRUE(writeResult.success);

    // A synchronous read must see the async worker's write (shared store).
    QueryResult readResult = pool.SyncQuery(kGetAsync);
    EXPECT_TRUE(readResult.success);
    EXPECT_TRUE(readResult.HasRows());
    if (readResult.HasRows())
    {
        EXPECT_EQ(readResult.rows[0].GetString(0), std::string("aval"));
    }

    pool.Close();
    std::filesystem::remove(path);
}

TEST(AsyncDatabasePool_AsyncAndSyncWrites_SurviveCloseReopen)
{
    const std::string path = MakeTempDbPath("persist");

    {
        AsyncDatabasePool pool;
        pool.PrepareStatement(kSetAsync, "SET akey aval");
        pool.PrepareStatement(kSetSync, "SET skey sval");
        EXPECT_TRUE(pool.Open(path, 2));

        auto future = pool.AsyncQuery(kSetAsync); // async worker write
        EXPECT_TRUE(future.get().success);

        QueryResult syncWrite = pool.SyncQuery(kSetSync); // sync write
        EXPECT_TRUE(syncWrite.success);

        pool.Close(); // flush to disk
    }

    // Reopen a fresh pool over the same file: BOTH keys must still be present.
    // Previously the sync connection's final flush wiped the async-written key.
    {
        AsyncDatabasePool pool;
        pool.PrepareStatement(kGetAsync, "GET akey");
        pool.PrepareStatement(kGetSync, "GET skey");
        EXPECT_TRUE(pool.Open(path, 2));

        QueryResult a = pool.SyncQuery(kGetAsync);
        EXPECT_TRUE(a.success);
        EXPECT_TRUE(a.HasRows());
        if (a.HasRows())
        {
            EXPECT_EQ(a.rows[0].GetString(0), std::string("aval"));
        }

        QueryResult s = pool.SyncQuery(kGetSync);
        EXPECT_TRUE(s.success);
        EXPECT_TRUE(s.HasRows());
        if (s.HasRows())
        {
            EXPECT_EQ(s.rows[0].GetString(0), std::string("sval"));
        }

        pool.Close();
    }

    std::filesystem::remove(path);
}
