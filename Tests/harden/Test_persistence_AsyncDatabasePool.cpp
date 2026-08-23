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

#include <atomic>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <future>
#include <string>
#include <thread>
#include <vector>

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

TEST(AsyncDatabasePool_CloseRace_DrainsAcceptedAndCompletesRejectedWork)
{
    using namespace std::chrono_literals;

    const std::string path = MakeTempDbPath("close_enqueue_race");
    AsyncDatabasePool pool;
    pool.PrepareStatement(kSetAsync, "SET race value");
    pool.PrepareStatement(kGetAsync, "GET race");
    EXPECT_TRUE(pool.Open(path, 1));

    // Seed enough accepted work to exercise Close's drain contract rather than
    // merely the already-closed fast path.
    std::vector<std::future<QueryResult>> acceptedBeforeClose;
    acceptedBeforeClose.reserve(32);
    for (int i = 0; i < 32; ++i)
        acceptedBeforeClose.push_back(pool.AsyncQuery(kSetAsync));

    std::future<QueryResult> racingQuery;
    std::future<QueryResult> racingTransaction;
    std::atomic<int> callbackCount{0};
    std::barrier startRace(3);

    std::thread producer(
        [&]
        {
            startRace.arrive_and_wait();
            racingQuery = pool.AsyncQuery(kGetAsync);
            pool.AsyncQueryWithCallback(kGetAsync, {},
                                        [&](QueryResult) { callbackCount.fetch_add(1, std::memory_order_relaxed); });
            Transaction transaction;
            transaction.Append(kSetAsync);
            racingTransaction = pool.AsyncTransaction(std::move(transaction));
        });
    std::thread closer(
        [&]
        {
            startRace.arrive_and_wait();
            pool.Close();
        });

    startRace.arrive_and_wait();
    producer.join();
    closer.join();

    for (auto& future : acceptedBeforeClose)
    {
        EXPECT_TRUE(future.wait_for(2s) == std::future_status::ready);
        if (future.wait_for(0s) == std::future_status::ready)
            EXPECT_TRUE(future.get().success);
    }

    // Racing operations are allowed either outcome at the admission boundary:
    // accepted work executes; rejected work completes with the normal closed
    // result. Neither outcome may strand a future or callback.
    EXPECT_TRUE(racingQuery.valid());
    if (racingQuery.valid())
        EXPECT_TRUE(racingQuery.wait_for(2s) == std::future_status::ready);
    EXPECT_TRUE(racingTransaction.valid());
    if (racingTransaction.valid())
        EXPECT_TRUE(racingTransaction.wait_for(2s) == std::future_status::ready);

    pool.ProcessCallbacks();
    EXPECT_EQ(callbackCount.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(pool.GetPendingQueryCount(), 0);
    EXPECT_FALSE(pool.IsOpen());

    std::filesystem::remove(path);
}
