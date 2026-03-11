/**
 * @file TestReplaySystem.cpp
 * @brief Tests for Spark::ReplaySystem
 */

#include "TestFramework.h"
#include "../SparkEngine/Source/Engine/Replay/ReplaySystem.h"

TEST(Replay_RecordFrames)
{
    Spark::ReplaySystem replay;
    replay.StartRecording();
    EXPECT_TRUE(replay.IsRecording());

    std::vector<Spark::ReplayEntityState> entities;
    Spark::ReplayEntityState e;
    e.entityId = 1;
    e.position = {1.0f, 0.0f, 0.0f};
    e.health = 100.0f;
    entities.push_back(e);

    replay.RecordFrame(entities, 0.0f);
    replay.RecordFrame(entities, 0.05f);
    replay.RecordFrame(entities, 0.10f);

    replay.StopRecording();
    EXPECT_FALSE(replay.IsRecording());
    EXPECT_NEAR(replay.GetDuration(), 0.10f, 0.001f);
}

TEST(Replay_RecordEvents)
{
    Spark::ReplaySystem replay;
    replay.StartRecording();

    Spark::ReplayEvent event;
    event.timestamp = 1.5f;
    event.type = "kill";
    event.sourceEntity = 1;
    event.targetEntity = 2;
    replay.RecordEvent(event);

    replay.StopRecording();

    // Start playback and check events
    replay.StartPlayback();
    replay.SeekTo(1.5f);
    auto events = replay.GetEventsNearTime(0.5f);
    EXPECT_EQ(events.size(), static_cast<size_t>(1));
    EXPECT_EQ(events[0].type, std::string("kill"));
}

TEST(Replay_Playback)
{
    Spark::ReplaySystem replay;
    replay.StartRecording();

    std::vector<Spark::ReplayEntityState> entities;
    Spark::ReplayEntityState e;
    e.entityId = 1;
    entities.push_back(e);

    for (int i = 0; i < 20; ++i)
    {
        entities[0].position.x = static_cast<float>(i);
        replay.RecordFrame(entities, static_cast<float>(i) * 0.05f);
    }
    replay.StopRecording();

    replay.StartPlayback();
    EXPECT_EQ(replay.GetPlaybackState(), Spark::PlaybackState::Playing);

    replay.SetPlaybackSpeed(2.0f);
    EXPECT_NEAR(replay.GetPlaybackSpeed(), 2.0f, 0.001f);

    replay.PausePlayback();
    EXPECT_EQ(replay.GetPlaybackState(), Spark::PlaybackState::Paused);

    replay.StopPlayback();
    EXPECT_EQ(replay.GetPlaybackState(), Spark::PlaybackState::Stopped);
}

TEST(Replay_KillCam)
{
    Spark::ReplaySystem replay;
    replay.StartRecording();

    std::vector<Spark::ReplayEntityState> entities(1);
    entities[0].entityId = 1;

    for (int i = 0; i < 100; ++i)
    {
        replay.RecordFrame(entities, static_cast<float>(i) * 0.05f);
    }
    replay.StopRecording();

    replay.StartKillCam(2.0f, 1);
    EXPECT_TRUE(replay.IsKillCamActive());
    EXPECT_EQ(replay.GetCamera(), Spark::PlaybackCamera::KillCam);
    EXPECT_NEAR(replay.GetPlaybackSpeed(), 0.5f, 0.001f);

    replay.StopKillCam();
    EXPECT_FALSE(replay.IsKillCamActive());
}
