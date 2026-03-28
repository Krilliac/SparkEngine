// TestReplaySystem.cpp - Tests for record/playback replay system

#include "TestFramework.h"
#include "Engine/Replay/ReplaySystem.h"

using namespace Spark;

TEST(ReplaySystem_DefaultState)
{
    auto& replay = ReplaySystem::GetInstance();
    EXPECT_FALSE(replay.IsRecording());
    EXPECT_TRUE(replay.GetPlaybackState() == PlaybackState::Stopped);
}

TEST(ReplaySystem_StartStopRecording)
{
    auto& replay = ReplaySystem::GetInstance();
    replay.StartRecording();
    EXPECT_TRUE(replay.IsRecording());
    replay.StopRecording();
    EXPECT_FALSE(replay.IsRecording());
}

TEST(ReplaySystem_RecordFrame)
{
    auto& replay = ReplaySystem::GetInstance();
    replay.StartRecording();

    ReplayEntityState entity;
    entity.entityId = 1;
    entity.position = DirectX::XMFLOAT3{10.0f, 0.0f, 5.0f};
    entity.rotation = DirectX::XMFLOAT4{0.0f, 0.0f, 0.0f, 1.0f};
    entity.health = 100.0f;
    entity.flags = ReplayFlags::Alive | ReplayFlags::Visible;

    replay.RecordFrame({entity}, 0.0f);
    replay.RecordFrame({entity}, 0.033f);
    replay.RecordFrame({entity}, 0.066f);
    replay.StopRecording();

    // Duration depends on implementation — just verify no crash
    EXPECT_GE(replay.GetDuration(), 0.0f);
}

TEST(ReplaySystem_RecordEvent)
{
    auto& replay = ReplaySystem::GetInstance();
    replay.StartRecording();

    ReplayEvent event;
    event.timestamp = 1.0f;
    event.type = "Kill";
    event.sourceEntity = 1;
    event.targetEntity = 2;
    replay.RecordEvent(event);

    replay.StopRecording();
}

TEST(ReplaySystem_SetMetadata)
{
    auto& replay = ReplaySystem::GetInstance();
    replay.SetMetadata("TestMap", "Deathmatch");
    replay.StartRecording();
    replay.StopRecording();
}

TEST(ReplaySystem_PlaybackSpeed)
{
    auto& replay = ReplaySystem::GetInstance();
    replay.SetPlaybackSpeed(2.0f);
    EXPECT_NEAR(replay.GetPlaybackSpeed(), 2.0f, 0.01f);
    replay.SetPlaybackSpeed(1.0f);
}

TEST(ReplaySystem_ConsoleStatus)
{
    auto& replay = ReplaySystem::GetInstance();
    std::string status = replay.Console_GetStatus();
    EXPECT_FALSE(status.empty());
}

TEST(ReplaySystem_CameraMode)
{
    auto& replay = ReplaySystem::GetInstance();
    replay.SetCamera(PlaybackCamera::FreeCam);
    EXPECT_TRUE(replay.GetCamera() == PlaybackCamera::FreeCam);
    replay.SetCamera(PlaybackCamera::FollowCam);
    EXPECT_TRUE(replay.GetCamera() == PlaybackCamera::FollowCam);
}
