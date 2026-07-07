/**
 * @file Test_engine-misc_Coroutine.cpp
 * @brief Regression tests for coroutine scheduler reentrancy and cancellation chains.
 *
 * - CoroutineScheduler::Update must tolerate a coroutine that starts more
 *   coroutines mid-tick (which push_back onto — and can reallocate — the very
 *   vector being iterated). Newly spawned coroutines are deferred to the next
 *   frame rather than corrupting the walk.
 * - CancellationToken::CreateLinked must produce a token that observes
 *   cancellation of ANY ancestor, not just its immediate parent.
 */

#include "../TestFramework.h"
#include "Engine/Coroutine/CoroutineScheduler.h"
#include "Engine/Coroutine/CancellationToken.h"

#include <string>

TEST(CoroutineScheduler_SpawnDuringTickIsSafe)
{
    auto& sched = Spark::CoroutineScheduler::GetInstance();

    // Start from a clean slate (other tests share this singleton).
    sched.StopAll();
    sched.Update(0.0f);
    EXPECT_EQ(sched.ActiveCount(), static_cast<size_t>(0));

    constexpr int kChildren = 64;
    int childrenRun = 0;

    // A coroutine that, while ticking, spawns many more coroutines. In the old
    // range-for implementation this reallocated the vector under the iterator
    // (heap-use-after-free); the index-over-cached-size fix defers the new
    // coroutines to the next frame instead.
    sched.StartCoroutine("spawner").Do(
        [&sched, &childrenRun]()
        {
            for (int i = 0; i < kChildren; ++i)
            {
                sched.StartCoroutine("child_" + std::to_string(i)).Do([&childrenRun]() { ++childrenRun; });
            }
        });

    // First tick: spawner runs and finishes; the 64 children were added mid-tick
    // and are deferred, so none have run yet.
    sched.Update(0.016f);
    EXPECT_EQ(childrenRun, 0);
    EXPECT_EQ(sched.ActiveCount(), static_cast<size_t>(kChildren));

    // Second tick: the deferred children run.
    sched.Update(0.016f);
    EXPECT_EQ(childrenRun, kChildren);
    EXPECT_EQ(sched.ActiveCount(), static_cast<size_t>(0));
}

TEST(CancellationToken_LinkedWalksFullAncestorChain)
{
    Spark::CancellationToken root;
    Spark::CancellationToken child = root.CreateLinked();
    Spark::CancellationToken grandchild = child.CreateLinked();

    EXPECT_FALSE(grandchild.IsCancelledOrParent());

    // Cancelling the root (two levels up) must be observed by the grandchild.
    root.Cancel();
    EXPECT_TRUE(grandchild.IsCancelledOrParent());
    EXPECT_TRUE(child.IsCancelledOrParent());

    // The grandchild's own flag was never set directly.
    EXPECT_FALSE(grandchild.IsCancelled());
}
