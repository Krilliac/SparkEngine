/**
 * @file TestLoadingScreen.cpp
 * @brief Tests for Spark::LoadingScreen
 */

#include "TestFramework.h"
#include "../SparkEngine/Source/Engine/Loading/LoadingScreen.h"

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
    EXPECT_EQ(loader.GetState(), Spark::LoadingState::Completed);
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

    EXPECT_EQ(loader.GetState(), Spark::LoadingState::Failed);
}

TEST(Loading_Tips)
{
    Spark::LoadingScreen loader;
    loader.AddLoadingTip("Tip 1");
    loader.AddLoadingTip("Tip 2");

    std::string tip = loader.GetCurrentTip();
    EXPECT_TRUE(tip == "Tip 1" || tip == "Tip 2");
}
