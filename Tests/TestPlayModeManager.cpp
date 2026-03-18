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
    EXPECT_TRUE(status.contains("Stopped"));

    pm.EnterPlayMode();
    status = pm.Console_GetStatus();
    EXPECT_TRUE(status.contains("Playing"));

    pm.ExitPlayMode();
}

// ============================================================================
// New feature tests: Simulation subsystems, camera, live editing, multi-step
// ============================================================================

TEST(PlayMode_SimulationSubsystems_Default)
{
    Spark::Editor::PlayModeManager pm;
    // By default all subsystems should be enabled
    EXPECT_TRUE(pm.IsSubsystemEnabled(Spark::Editor::SimulationSubsystem::Physics));
    EXPECT_TRUE(pm.IsSubsystemEnabled(Spark::Editor::SimulationSubsystem::AI));
    EXPECT_TRUE(pm.IsSubsystemEnabled(Spark::Editor::SimulationSubsystem::Audio));
    EXPECT_TRUE(pm.IsSubsystemEnabled(Spark::Editor::SimulationSubsystem::Animation));
    EXPECT_TRUE(pm.IsSubsystemEnabled(Spark::Editor::SimulationSubsystem::Scripting));
    EXPECT_TRUE(pm.IsSubsystemEnabled(Spark::Editor::SimulationSubsystem::Particles));
}

TEST(PlayMode_SimulationSubsystems_Toggle)
{
    Spark::Editor::PlayModeManager pm;

    pm.SetSubsystemEnabled(Spark::Editor::SimulationSubsystem::Physics, false);
    EXPECT_FALSE(pm.IsSubsystemEnabled(Spark::Editor::SimulationSubsystem::Physics));
    EXPECT_TRUE(pm.IsSubsystemEnabled(Spark::Editor::SimulationSubsystem::AI));

    pm.SetSubsystemEnabled(Spark::Editor::SimulationSubsystem::Physics, true);
    EXPECT_TRUE(pm.IsSubsystemEnabled(Spark::Editor::SimulationSubsystem::Physics));
}

TEST(PlayMode_SimulationSubsystems_Callback)
{
    Spark::Editor::PlayModeManager pm;
    Spark::Editor::SimulationSubsystem toggledSys = Spark::Editor::SimulationSubsystem::None;
    bool toggledEnabled = false;

    Spark::Editor::PlayModeCallbacks callbacks;
    callbacks.onSubsystemToggled = [&](Spark::Editor::SimulationSubsystem sys, bool enabled)
    {
        toggledSys = sys;
        toggledEnabled = enabled;
    };
    pm.SetCallbacks(callbacks);

    pm.SetSubsystemEnabled(Spark::Editor::SimulationSubsystem::AI, false);
    EXPECT_TRUE(toggledSys == Spark::Editor::SimulationSubsystem::AI);
    EXPECT_FALSE(toggledEnabled);
}

TEST(PlayMode_SubsystemStatusString)
{
    Spark::Editor::PlayModeManager pm;
    std::string status = pm.GetSubsystemStatusString();
    EXPECT_TRUE(status.contains("[Physics]"));
    EXPECT_TRUE(status.contains("[AI]"));

    pm.SetSubsystemEnabled(Spark::Editor::SimulationSubsystem::AI, false);
    status = pm.GetSubsystemStatusString();
    EXPECT_TRUE(!status.contains("[AI]"));
    EXPECT_TRUE(status.contains("[Physics]"));
}

TEST(PlayMode_CameraMode_Default)
{
    Spark::Editor::PlayModeManager pm;
    EXPECT_TRUE(pm.GetCameraMode() == Spark::Editor::EditorCameraMode::EditorFree);
}

TEST(PlayMode_CameraMode_Switch)
{
    Spark::Editor::PlayModeManager pm;
    Spark::Editor::EditorCameraMode reportedMode = Spark::Editor::EditorCameraMode::EditorFree;

    Spark::Editor::PlayModeCallbacks callbacks;
    callbacks.onCameraModeChanged = [&](Spark::Editor::EditorCameraMode mode) { reportedMode = mode; };
    pm.SetCallbacks(callbacks);

    pm.SetCameraMode(Spark::Editor::EditorCameraMode::GameCamera);
    EXPECT_TRUE(pm.GetCameraMode() == Spark::Editor::EditorCameraMode::GameCamera);
    EXPECT_TRUE(reportedMode == Spark::Editor::EditorCameraMode::GameCamera);

    pm.SetCameraMode(Spark::Editor::EditorCameraMode::FollowPlayer);
    EXPECT_TRUE(pm.GetCameraMode() == Spark::Editor::EditorCameraMode::FollowPlayer);
}

TEST(PlayMode_CameraMode_ResetOnExit)
{
    Spark::Editor::PlayModeManager pm;
    Spark::Editor::PlayModeConfig config;
    config.cameraMode = Spark::Editor::EditorCameraMode::GameCamera;
    pm.SetConfig(config);

    pm.EnterPlayMode();
    EXPECT_TRUE(pm.GetCameraMode() == Spark::Editor::EditorCameraMode::GameCamera);

    pm.ExitPlayMode();
    // Camera should reset to EditorFree on exit
    EXPECT_TRUE(pm.GetCameraMode() == Spark::Editor::EditorCameraMode::EditorFree);
}

TEST(PlayMode_LiveEditing_Disabled)
{
    Spark::Editor::PlayModeManager pm;
    pm.EnterPlayMode();

    // Live editing is disabled by default
    EXPECT_FALSE(pm.IsLiveEditingEnabled());
    EXPECT_FALSE(pm.RecordLiveEdit(1, "Transform", "position.x", "0", "5"));
    EXPECT_EQ(pm.GetLiveEditCount(), static_cast<size_t>(0));

    pm.ExitPlayMode();
}

TEST(PlayMode_LiveEditing_Enabled)
{
    Spark::Editor::PlayModeManager pm;
    pm.SetLiveEditingEnabled(true);
    pm.EnterPlayMode();
    pm.Update(0.016f); // Advance time a bit

    EXPECT_TRUE(pm.IsLiveEditingEnabled());
    EXPECT_TRUE(pm.RecordLiveEdit(1, "Transform", "position.x", "0", "5"));
    EXPECT_TRUE(pm.RecordLiveEdit(1, "Transform", "position.y", "0", "10"));
    EXPECT_EQ(pm.GetLiveEditCount(), static_cast<size_t>(2));

    const auto& edits = pm.GetLiveEdits();
    EXPECT_EQ(edits[0].entityId, static_cast<uint32_t>(1));
    EXPECT_EQ(edits[0].componentType, std::string("Transform"));
    EXPECT_EQ(edits[0].propertyName, std::string("position.x"));
    EXPECT_EQ(edits[0].oldValue, std::string("0"));
    EXPECT_EQ(edits[0].newValue, std::string("5"));

    pm.ExitPlayMode();
}

TEST(PlayMode_LiveEditing_Callback)
{
    Spark::Editor::PlayModeManager pm;
    pm.SetLiveEditingEnabled(true);

    int editCallbackCount = 0;
    Spark::Editor::PlayModeCallbacks callbacks;
    callbacks.onLiveEdit = [&](const Spark::Editor::LiveEditRecord&) { editCallbackCount++; };
    pm.SetCallbacks(callbacks);

    pm.EnterPlayMode();
    pm.RecordLiveEdit(1, "Transform", "x", "0", "1");
    pm.RecordLiveEdit(2, "Transform", "y", "0", "2");
    EXPECT_EQ(editCallbackCount, 2);

    pm.ExitPlayMode();
}

TEST(PlayMode_LiveEditing_NotInPlayMode)
{
    Spark::Editor::PlayModeManager pm;
    pm.SetLiveEditingEnabled(true);
    // Not in play mode — should fail
    EXPECT_FALSE(pm.RecordLiveEdit(1, "Transform", "x", "0", "1"));
}

TEST(PlayMode_LiveEditing_ClearedOnEnter)
{
    Spark::Editor::PlayModeManager pm;
    pm.SetLiveEditingEnabled(true);

    pm.EnterPlayMode();
    pm.RecordLiveEdit(1, "T", "x", "0", "1");
    EXPECT_EQ(pm.GetLiveEditCount(), static_cast<size_t>(1));
    pm.ExitPlayMode();

    // Second play session should start clean
    pm.EnterPlayMode();
    EXPECT_EQ(pm.GetLiveEditCount(), static_cast<size_t>(0));
    pm.ExitPlayMode();
}

TEST(PlayMode_KeepChangesOnStop)
{
    Spark::Editor::PlayModeManager pm;
    pm.SetKeepChangesOnStop(true);
    EXPECT_TRUE(pm.GetKeepChangesOnStop());

    pm.EnterPlayMode();
    EXPECT_TRUE(pm.HasSnapshot());

    pm.ExitPlayMode();
    // Should still exit cleanly
    EXPECT_TRUE(pm.IsStopped());
}

TEST(PlayMode_MultiStep)
{
    Spark::Editor::PlayModeManager pm;
    pm.EnterPlayMode();
    pm.PausePlayMode();

    // Request 3 steps
    pm.StepFrames(3);
    EXPECT_EQ(pm.GetMultiStepRemaining(), static_cast<uint32_t>(3));

    pm.Update(0.016f);
    EXPECT_EQ(pm.GetFrameCount(), static_cast<uint64_t>(1));
    EXPECT_EQ(pm.GetMultiStepRemaining(), static_cast<uint32_t>(2));

    pm.Update(0.016f);
    EXPECT_EQ(pm.GetFrameCount(), static_cast<uint64_t>(2));
    EXPECT_EQ(pm.GetMultiStepRemaining(), static_cast<uint32_t>(1));

    pm.Update(0.016f);
    EXPECT_EQ(pm.GetFrameCount(), static_cast<uint64_t>(3));
    EXPECT_EQ(pm.GetMultiStepRemaining(), static_cast<uint32_t>(0));

    // No more stepping — should stop
    pm.Update(0.016f);
    EXPECT_EQ(pm.GetFrameCount(), static_cast<uint64_t>(3));

    pm.ExitPlayMode();
}

TEST(PlayMode_MultiStep_ZeroNoOp)
{
    Spark::Editor::PlayModeManager pm;
    pm.EnterPlayMode();
    pm.PausePlayMode();

    pm.StepFrames(0);
    pm.Update(0.016f);
    EXPECT_EQ(pm.GetFrameCount(), static_cast<uint64_t>(0));

    pm.ExitPlayMode();
}

TEST(PlayMode_Stats_Reset)
{
    Spark::Editor::PlayModeManager pm;
    pm.EnterPlayMode();
    pm.Update(0.016f);
    pm.Update(0.020f);

    auto stats = pm.GetStats();
    EXPECT_TRUE(stats.physicsSteps > 0);
    EXPECT_TRUE(stats.averageFPS > 0.0f);

    pm.ExitPlayMode();

    // Stats should reset on next enter
    pm.EnterPlayMode();
    stats = pm.GetStats();
    EXPECT_EQ(stats.physicsSteps, static_cast<uint32_t>(0));
    EXPECT_NEAR(stats.averageFPS, 0.0f, 0.001f);
    pm.ExitPlayMode();
}

TEST(PlayMode_Stats_SubsystemTracking)
{
    Spark::Editor::PlayModeManager pm;
    pm.EnterPlayMode();

    // With all subsystems enabled, stats should track
    pm.Update(0.016f);
    auto stats = pm.GetStats();
    EXPECT_EQ(stats.physicsSteps, static_cast<uint32_t>(1));
    EXPECT_EQ(stats.aiUpdates, static_cast<uint32_t>(1));
    EXPECT_EQ(stats.scriptExecutions, static_cast<uint32_t>(1));

    // Disable physics, AI
    pm.SetSubsystemEnabled(Spark::Editor::SimulationSubsystem::Physics, false);
    pm.SetSubsystemEnabled(Spark::Editor::SimulationSubsystem::AI, false);
    pm.Update(0.016f);
    stats = pm.GetStats();
    // Physics and AI should not have incremented again
    EXPECT_EQ(stats.physicsSteps, static_cast<uint32_t>(1));
    EXPECT_EQ(stats.aiUpdates, static_cast<uint32_t>(1));
    // Script should have incremented
    EXPECT_EQ(stats.scriptExecutions, static_cast<uint32_t>(2));

    pm.ExitPlayMode();
}

TEST(PlayMode_CurrentFPS)
{
    Spark::Editor::PlayModeManager pm;
    EXPECT_NEAR(pm.GetCurrentFPS(), 0.0f, 0.001f); // No play time

    pm.EnterPlayMode();
    pm.Update(0.016f);
    pm.Update(0.016f);
    // Should be approximately 2 frames / 0.032s ~= 62.5 FPS
    EXPECT_GT(pm.GetCurrentFPS(), 50.0f);

    pm.ExitPlayMode();
}

TEST(PlayMode_ConsoleStatus_Extended)
{
    Spark::Editor::PlayModeManager pm;
    pm.SetLiveEditingEnabled(true);
    pm.EnterPlayMode();
    pm.Update(0.016f);
    pm.RecordLiveEdit(1, "T", "x", "0", "1");

    std::string status = pm.Console_GetStatus();
    EXPECT_TRUE(status.contains("FPS:"));
    EXPECT_TRUE(status.contains("LiveEdits: 1"));

    pm.ExitPlayMode();
}
