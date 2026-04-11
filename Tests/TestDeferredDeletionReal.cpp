/**
 * @file TestDeferredDeletionReal.cpp
 * @brief Real-class tests for Spark::RHI::DeferredDeletionQueue
 */

#include "TestFramework.h"
#include "Graphics/RHI/DeferredDeletionQueue.h"

TEST(DeferredDeletionReal_ConstructPendingZero)
{
    Spark::RHI::DeferredDeletionQueue q;
    EXPECT_EQ(q.GetPendingCount(), static_cast<size_t>(0));
    EXPECT_EQ(q.GetCurrentFrame(), static_cast<uint64_t>(0));
}

TEST(DeferredDeletionReal_EnqueueIncreasesPending)
{
    Spark::RHI::DeferredDeletionQueue q(3);
    bool deleted = false;
    q.Enqueue([&]() { deleted = true; });
    EXPECT_EQ(q.GetPendingCount(), static_cast<size_t>(1));
    EXPECT_FALSE(deleted);
}

TEST(DeferredDeletionReal_ProcessQueueDefers)
{
    Spark::RHI::DeferredDeletionQueue q(3);
    int count = 0;
    q.Enqueue([&]() { ++count; });

    // Frames 1, 2 — still deferring.
    q.ProcessQueue();
    q.ProcessQueue();
    EXPECT_EQ(count, 0);

    // Frame 3 — delete.
    q.ProcessQueue();
    EXPECT_EQ(count, 1);
    EXPECT_EQ(q.GetPendingCount(), static_cast<size_t>(0));
}

TEST(DeferredDeletionReal_FlushAllImmediate)
{
    Spark::RHI::DeferredDeletionQueue q(3);
    int count = 0;
    q.Enqueue([&]() { ++count; });
    q.Enqueue([&]() { ++count; });
    q.Enqueue([&]() { ++count; });

    q.FlushAll();
    EXPECT_EQ(count, 3);
    EXPECT_EQ(q.GetPendingCount(), static_cast<size_t>(0));
}

TEST(DeferredDeletionReal_CurrentFrameIncrements)
{
    Spark::RHI::DeferredDeletionQueue q;
    EXPECT_EQ(q.GetCurrentFrame(), static_cast<uint64_t>(0));
    q.ProcessQueue();
    EXPECT_EQ(q.GetCurrentFrame(), static_cast<uint64_t>(1));
    q.ProcessQueue();
    EXPECT_EQ(q.GetCurrentFrame(), static_cast<uint64_t>(2));
}

TEST(DeferredDeletionReal_ZeroFrameDeferImmediate)
{
    Spark::RHI::DeferredDeletionQueue q(0);
    int count = 0;
    q.Enqueue([&]() { ++count; });
    q.ProcessQueue();
    EXPECT_EQ(count, 1);
}
