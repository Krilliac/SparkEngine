// TestDeferredDeletion.cpp - Tests for frame-delayed GPU resource destruction
// Validates that resources are not freed until N frames after Destroy() is called.
//
// These tests drive the production Spark::RHI::DeferredDeletionQueue directly.
// The file previously carried a local copy of the class, so it could not have
// caught a regression in the real one.

#include "TestFramework.h"
#include "Graphics/RHI/DeferredDeletionQueue.h"

#include <cstdint>
#include <vector>

using Spark::RHI::DeferredDeletionQueue;

TEST(DeferredDeletion_ResourceNotDeletedImmediately)
{
    DeferredDeletionQueue queue(3);
    bool deleted = false;

    queue.Enqueue([&deleted] { deleted = true; });

    EXPECT_FALSE(deleted);
    EXPECT_EQ(queue.GetPendingCount(), 1u);
}

TEST(DeferredDeletion_ResourceDeletedAfterNFrames)
{
    DeferredDeletionQueue queue(3);
    bool deleted = false;

    queue.Enqueue([&deleted] { deleted = true; });

    // Simulate 2 frames — should still be pending
    queue.ProcessQueue(); // frame 1
    queue.ProcessQueue(); // frame 2
    EXPECT_FALSE(deleted);
    EXPECT_EQ(queue.GetPendingCount(), 1u);

    // Frame 3 — should be deleted now
    queue.ProcessQueue();
    EXPECT_TRUE(deleted);
    EXPECT_EQ(queue.GetPendingCount(), 0u);
}

TEST(DeferredDeletion_MultipleResourcesDeletedInOrder)
{
    DeferredDeletionQueue queue(2);
    std::vector<int> deletionOrder;

    queue.Enqueue([&deletionOrder] { deletionOrder.push_back(1); });
    queue.ProcessQueue(); // frame 1
    queue.Enqueue([&deletionOrder] { deletionOrder.push_back(2); });

    queue.ProcessQueue(); // frame 2 — first resource should be deleted
    EXPECT_EQ(deletionOrder.size(), 1u);
    EXPECT_EQ(deletionOrder[0], 1);

    queue.ProcessQueue(); // frame 3 — second resource deleted
    EXPECT_EQ(deletionOrder.size(), 2u);
    EXPECT_EQ(deletionOrder[1], 2);
}

TEST(DeferredDeletion_FlushAllDeletesEverythingImmediately)
{
    DeferredDeletionQueue queue(10); // Long defer
    int deleteCount = 0;

    queue.Enqueue([&deleteCount] { deleteCount++; });
    queue.Enqueue([&deleteCount] { deleteCount++; });
    queue.Enqueue([&deleteCount] { deleteCount++; });

    EXPECT_EQ(deleteCount, 0);
    queue.FlushAll();
    EXPECT_EQ(deleteCount, 3);
    EXPECT_EQ(queue.GetPendingCount(), 0u);
}

TEST(DeferredDeletion_ZeroDeferDeletesNextFrame)
{
    DeferredDeletionQueue queue(0);
    bool deleted = false;

    queue.Enqueue([&deleted] { deleted = true; });
    EXPECT_FALSE(deleted); // Not yet — ProcessQueue hasn't been called

    queue.ProcessQueue();
    EXPECT_TRUE(deleted); // Deleted on first ProcessQueue
}

TEST(DeferredDeletion_NullEnqueueSafe)
{
    DeferredDeletionQueue queue(1);
    // Enqueue no-op deletion — should not crash
    queue.Enqueue([] {});
    queue.ProcessQueue();
    EXPECT_EQ(queue.GetPendingCount(), 0u);
}
