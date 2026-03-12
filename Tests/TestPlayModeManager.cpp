/**
 * @file TestPlayModeManager.cpp
 * @brief Tests for Spark::Editor::PlayModeManager
 */

#include "TestFramework.h"
#include "../SparkEngine/Source/Engine/Editor/PlayModeManager.h"

TEST(PlayMode_InitialState)
{
    Spark::Editor::PlayModeManager pm;
    EXPECT_TRUE(pm.IsStopped());
    EXPECT_FALSE(pm.IsPlaying());
    EXPECT_FALSE(pm.IsPaused());
    EXPECT_FALSE(pm.IsInPlayMode());
    EXPECT_FALSE(pm.HasSnapshot());
    EXPECT_EQ(pm.GetFrameCount(), static_cast<uint64_t>(0));
    EXPECT_NEAR(pm.GetPlayTime(), 0.0f, 0.001f);
}

TEST(PlayMode_EnterExit)
{
    Spark::Editor::PlayModeManager pm;

    EXPECT_TRUE(pm.EnterPlayMode());
    EXPECT_TRUE(pm.IsPlaying());
    EXPECT_TRUE(pm.IsInPlayMode());
    EXPECT_TRUE(pm.HasSnapshot());

    EXPECT_TRUE(pm.ExitPlayMode());
    EXPECT_TRUE(pm.IsStopped());
    EXPECT_FALSE(pm.IsInPlayMode());
}

TEST(PlayMode_DoubleEnterFails)
{
    Spark::Editor::PlayModeManager pm;
    EXPECT_TRUE(pm.EnterPlayMode());
    EXPECT_FALSE(pm.EnterPlayMode()); // Already playing
    pm.ExitPlayMode();
}

TEST(PlayMode_ExitWhenStoppedFails)
{
    Spark::Editor::PlayModeManager pm;
    EXPECT_FALSE(pm.ExitPlayMode()); // Not in play mode
}

TEST(PlayMode_Toggle)
{
    Spark::Editor::PlayModeManager pm;

    pm.TogglePlayMode();
    EXPECT_TRUE(pm.IsPlaying());

    pm.TogglePlayMode();
    EXPECT_TRUE(pm.IsStopped());
}

TEST(PlayMode_PauseResume)
{
    Spark::Editor::PlayModeManager pm;
    pm.EnterPlayMode();

    pm.PausePlayMode();
    EXPECT_TRUE(pm.IsPaused());
    EXPECT_TRUE(pm.IsInPlayMode());

    pm.ResumePlayMode();
    EXPECT_TRUE(pm.IsPlaying());

    pm.ExitPlayMode();
}

TEST(PlayMode_TogglePause)
{
    Spark::Editor::PlayModeManager pm;
    pm.EnterPlayMode();

    pm.TogglePause();
    EXPECT_TRUE(pm.IsPaused());

    pm.TogglePause();
    EXPECT_TRUE(pm.IsPlaying());

    pm.ExitPlayMode();
}

TEST(PlayMode_PauseWhenStoppedNoOp)
{
    Spark::Editor::PlayModeManager pm;
    pm.PausePlayMode(); // Should be a no-op
    EXPECT_TRUE(pm.IsStopped());
}

TEST(PlayMode_StepFrame)
{
    Spark::Editor::PlayModeManager pm;
    pm.EnterPlayMode();
    pm.PausePlayMode();

    // Update while paused does nothing
    pm.Update(0.016f);
    EXPECT_EQ(pm.GetFrameCount(), static_cast<uint64_t>(0));

    // Step advances exactly one frame
    pm.StepFrame();
    pm.Update(0.016f);
    EXPECT_EQ(pm.GetFrameCount(), static_cast<uint64_t>(1));

    // Without another step, no more advancement
    pm.Update(0.016f);
    EXPECT_EQ(pm.GetFrameCount(), static_cast<uint64_t>(1));

    pm.ExitPlayMode();
}

TEST(PlayMode_Update)
{
    Spark::Editor::PlayModeManager pm;
    pm.EnterPlayMode();

    pm.Update(0.016f);
    pm.Update(0.016f);
    pm.Update(0.016f);

    EXPECT_EQ(pm.GetFrameCount(), static_cast<uint64_t>(3));
    EXPECT_GT(pm.GetPlayTime(), 0.04f);

    pm.ExitPlayMode();
}

TEST(PlayMode_TimeScale)
{
    Spark::Editor::PlayModeManager pm;
    pm.SetTimeScale(2.0f);
    EXPECT_NEAR(pm.GetTimeScale(), 2.0f, 0.001f);

    pm.EnterPlayMode();
    pm.Update(0.016f); // Should accumulate 0.032s at 2x

    EXPECT_GT(pm.GetPlayTime(), 0.03f);
    pm.ExitPlayMode();

    // Clamp to [0, 10]
    pm.SetTimeScale(20.0f);
    EXPECT_NEAR(pm.GetTimeScale(), 10.0f, 0.001f);

    pm.SetTimeScale(-5.0f);
    EXPECT_NEAR(pm.GetTimeScale(), 0.0f, 0.001f);
}

TEST(PlayMode_StartPaused)
{
    Spark::Editor::PlayModeManager pm;
    Spark::Editor::PlayModeConfig config;
    config.startPaused = true;
    pm.SetConfig(config);

    pm.EnterPlayMode();
    EXPECT_TRUE(pm.IsPaused());

    pm.ExitPlayMode();
}

TEST(PlayMode_Callbacks)
{
    Spark::Editor::PlayModeManager pm;

    int enterCount = 0, exitCount = 0, pauseCount = 0, resumeCount = 0;
    Spark::Editor::PlayModeCallbacks callbacks;
    callbacks.onEnterPlay = [&]() { enterCount++; };
    callbacks.onExitPlay = [&]() { exitCount++; };
    callbacks.onPause = [&]() { pauseCount++; };
    callbacks.onResume = [&]() { resumeCount++; };
    pm.SetCallbacks(callbacks);

    pm.EnterPlayMode();
    EXPECT_EQ(enterCount, 1);

    pm.PausePlayMode();
    EXPECT_EQ(pauseCount, 1);

    pm.ResumePlayMode();
    EXPECT_EQ(resumeCount, 1);

    pm.ExitPlayMode();
    EXPECT_EQ(exitCount, 1);
}

TEST(PlayMode_ConsoleStatus)
{
    Spark::Editor::PlayModeManager pm;

    std::string status = pm.Console_GetStatus();
    EXPECT_TRUE(status.find("Stopped") != std::string::npos);

    pm.EnterPlayMode();
    status = pm.Console_GetStatus();
    EXPECT_TRUE(status.find("Playing") != std::string::npos);

    pm.ExitPlayMode();
}
