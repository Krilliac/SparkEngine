/**
 * @file TestLoadingScreen.cpp
 * @brief Tests for Spark::LoadingScreen
 */

#include "TestFramework.h"
#include "Engine/Loading/LoadingScreen.h"

TEST(Loading_BasicExecution)
{
    Spark::LoadingScreen loader;
    loader.BeginLoading("Test Level");

    int executedCount = 0;
    loader.AddTask("task1", 0.5f,
                   [&]()
                   {
                       executedCount++;
                       return true;
                   });
    loader.AddTask("task2", 0.5f,
                   [&]()
                   {
                       executedCount++;
                       return true;
                   });

    loader.Execute();

    EXPECT_EQ(executedCount, 2);
    EXPECT_TRUE(loader.GetState() == Spark::LoadingState::Completed);
    EXPECT_NEAR(loader.GetProgress(), 1.0f, 0.001f);
}

TEST(Loading_ProgressCallbacks)
{
    Spark::LoadingScreen loader;
    loader.BeginLoading("Test");

    std::vector<float> progressValues;
    loader.OnProgress([&](float p, const std::string&) { progressValues.push_back(p); });

    loader.AddTask("a", 1.0f, []() { return true; });
    loader.AddTask("b", 1.0f, []() { return true; });
    loader.AddTask("c", 1.0f, []() { return true; });

    loader.Execute();

    EXPECT_EQ(progressValues.size(), static_cast<size_t>(3));
    EXPECT_TRUE(progressValues[0] < progressValues[1]);
    EXPECT_TRUE(progressValues[1] < progressValues[2]);
    EXPECT_NEAR(progressValues[2], 1.0f, 0.001f);
}

TEST(Loading_FailedTask)
{
    Spark::LoadingScreen loader;
    loader.BeginLoading("Fail Test");

    loader.AddTask("ok", 0.5f, []() { return true; });
    loader.AddTask("fail", 0.5f, []() { return false; });

    loader.Execute();

    EXPECT_TRUE(loader.GetState() == Spark::LoadingState::Failed);
}

TEST(Loading_Tips)
{
    Spark::LoadingScreen loader;
    loader.AddLoadingTip("Tip 1");
    loader.AddLoadingTip("Tip 2");

    std::string tip = loader.GetCurrentTip();
    EXPECT_TRUE(tip == "Tip 1" || tip == "Tip 2");
}

TEST(Loading_EmptyTips)
{
    Spark::LoadingScreen loader;
    std::string tip = loader.GetCurrentTip();
    EXPECT_TRUE(tip.empty());
}

TEST(Loading_Cancel)
{
    Spark::LoadingScreen loader;
    loader.BeginLoading("Cancel Test");

    loader.AddTask("slow", 1.0f,
                   [&]()
                   {
                       loader.Cancel();
                       return true;
                   });
    loader.AddTask("after", 1.0f, []() { return true; });

    loader.Execute();

    EXPECT_TRUE(loader.GetState() == Spark::LoadingState::Cancelled);
}

TEST(Loading_CompletionCallback)
{
    Spark::LoadingScreen loader;
    loader.BeginLoading("Callback Test");

    bool completeCalled = false;
    bool completeSuccess = false;
    loader.OnComplete(
        [&](bool success)
        {
            completeCalled = true;
            completeSuccess = success;
        });

    loader.AddTask("task", 1.0f, []() { return true; });
    loader.Execute();

    EXPECT_TRUE(completeCalled);
    EXPECT_TRUE(completeSuccess);
}

TEST(Loading_SingleTask)
{
    Spark::LoadingScreen loader;
    loader.BeginLoading("Single");

    bool ran = false;
    loader.AddTask("only", 1.0f,
                   [&]()
                   {
                       ran = true;
                       return true;
                   });
    loader.Execute();

    EXPECT_TRUE(ran);
    EXPECT_NEAR(loader.GetProgress(), 1.0f, 0.001f);
    EXPECT_TRUE(loader.GetState() == Spark::LoadingState::Completed);
}

TEST(Loading_ConsoleStatus)
{
    Spark::LoadingScreen loader;
    loader.BeginLoading("Console Test");
    loader.AddTask("t1", 1.0f, []() { return true; });
    loader.Execute();

    std::string status = loader.Console_GetStatus();
    EXPECT_FALSE(status.empty());
}

TEST(Loading_BackgroundImage)
{
    Spark::LoadingScreen loader;
    loader.SetBackgroundImage("Data/Textures/loading.png");
    // Should not crash — configuration only
    loader.BeginLoading("BG Test");
    loader.AddTask("task", 1.0f, []() { return true; });
    loader.Execute();
    EXPECT_TRUE(loader.GetState() == Spark::LoadingState::Completed);
}

TEST(Loading_WeightedProgress)
{
    Spark::LoadingScreen loader;
    loader.BeginLoading("Weighted");

    std::vector<float> progressValues;
    loader.OnProgress([&](float p, const std::string&) { progressValues.push_back(p); });

    // First task has 3x the weight
    loader.AddTask("heavy", 3.0f, []() { return true; });
    loader.AddTask("light", 1.0f, []() { return true; });

    loader.Execute();

    EXPECT_EQ(progressValues.size(), static_cast<size_t>(2));
    // After heavy task: 3/4 = 0.75
    EXPECT_GT(progressValues[0], 0.5f);
    EXPECT_NEAR(progressValues[1], 1.0f, 0.001f);
}
