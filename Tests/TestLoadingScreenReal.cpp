/**
 * @file TestLoadingScreenReal.cpp
 * @brief Real-class tests for Spark::LoadingScreen
 */

#include "TestFramework.h"
#include "Engine/Loading/LoadingScreen.h"

TEST(LoadingScreenReal_DefaultConstruction)
{
    Spark::LoadingScreen ls;
    EXPECT_EQ(static_cast<int>(ls.GetState()), static_cast<int>(Spark::LoadingState::Idle));
    EXPECT_NEAR(ls.GetProgress(), 0.0f, 0.001f);
}

TEST(LoadingScreenReal_BeginLoadingIsSafe)
{
    Spark::LoadingScreen ls;
    ls.BeginLoading("TestLevel");
    // BeginLoading only prepares the session; the state transition
    // happens during Execute(). Just verify the call is safe.
    EXPECT_NEAR(ls.GetProgress(), 0.0f, 0.001f);
}

TEST(LoadingScreenReal_AddTaskAndExecute)
{
    Spark::LoadingScreen ls;
    ls.BeginLoading("TestLevel");
    int taskRunCount = 0;
    ls.AddTask("task1", 0.5f,
               [&]()
               {
                   ++taskRunCount;
                   return true;
               });
    ls.AddTask("task2", 0.5f,
               [&]()
               {
                   ++taskRunCount;
                   return true;
               });
    ls.Execute();
    EXPECT_EQ(taskRunCount, 2);
    EXPECT_NEAR(ls.GetProgress(), 1.0f, 0.01f);
}

TEST(LoadingScreenReal_AddLoadingTip)
{
    Spark::LoadingScreen ls;
    // With no tips registered the accessor returns an empty string rather than
    // indexing an empty container.
    EXPECT_TRUE(ls.GetCurrentTip().empty());

    ls.AddLoadingTip("Tip 1");
    ls.AddLoadingTip("Tip 2");
    const std::string tip = ls.GetCurrentTip();
    // The tip is picked at random, so assert membership rather than identity.
    EXPECT_FALSE(tip.empty());
    EXPECT_TRUE(tip == "Tip 1" || tip == "Tip 2");
}

TEST(LoadingScreenReal_CancelStopsExecution)
{
    Spark::LoadingScreen ls;
    ls.BeginLoading("TestLevel");
    int taskRuns = 0;
    ls.AddTask("task1", 1.0f,
               [&taskRuns]()
               {
                   ++taskRuns;
                   return true;
               });
    ls.Cancel();
    ls.Execute();

    // Execute polls the cancellation flag before each task, so a cancelled
    // session must run nothing and land in the Cancelled state.
    EXPECT_EQ(taskRuns, 0);
    EXPECT_EQ(static_cast<int>(ls.GetState()), static_cast<int>(Spark::LoadingState::Cancelled));
    EXPECT_NEAR(ls.GetProgress(), 0.0f, 0.001f);
}

TEST(LoadingScreenReal_SetMinimumDisplayTime)
{
    Spark::LoadingScreen ls;
    ls.SetMinimumDisplayTime(2.0f);
    ls.SetMinimumDisplayTime(0.5f);
    // LoadingScreen exposes no getter for the minimum display time, so the only
    // thing this can honestly claim is that the setter path is safe.
    EXPECT_NO_CRASH("SetMinimumDisplayTime has no observable accessor to assert on");
}
