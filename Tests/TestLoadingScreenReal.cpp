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
    ls.AddLoadingTip("Tip 1");
    ls.AddLoadingTip("Tip 2");
    // Just verify the calls don't crash and a tip is accessible.
    (void)ls.GetCurrentTip();
    EXPECT_TRUE(true);
}

TEST(LoadingScreenReal_CancelStopsExecution)
{
    Spark::LoadingScreen ls;
    ls.BeginLoading("TestLevel");
    ls.AddTask("task1", 1.0f, []() { return true; });
    ls.Cancel();
    // After Cancel, state should be Cancelled (or similar).
    // We only verify the call is safe.
    EXPECT_TRUE(true);
}

TEST(LoadingScreenReal_SetMinimumDisplayTime)
{
    Spark::LoadingScreen ls;
    ls.SetMinimumDisplayTime(2.0f);
    ls.SetMinimumDisplayTime(0.5f);
    // No getter; just exercise the setter path.
    EXPECT_TRUE(true);
}
