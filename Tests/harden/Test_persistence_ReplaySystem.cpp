// Test_persistence_ReplaySystem.cpp
// Regression for the P1 ReplaySystem data races: StartKillCam/StopKillCam/IsKillCamActive
// and the camera getters/setters used to touch shared members WITHOUT the mutex while
// recording/playback threads mutated the same state under the mutex. The fix guards every
// one of those members. This test verifies behavior is preserved AND hammers the guarded
// members concurrently so the TSan CI job has something to inspect.

#include "TestFramework.h"
#include "Engine/Replay/ReplaySystem.h"

#include <atomic>
#include <thread>

using namespace Spark;

TEST(ReplaySystem_KillCam_StateTransitionsPreserved)
{
    auto& replay = ReplaySystem::GetInstance();

    replay.StartKillCam(1.0f, 42u);
    EXPECT_TRUE(replay.IsKillCamActive());
    EXPECT_TRUE(replay.GetCamera() == PlaybackCamera::KillCam);
    EXPECT_TRUE(replay.GetPlaybackState() == PlaybackState::Playing);

    replay.StopKillCam();
    EXPECT_FALSE(replay.IsKillCamActive());
    EXPECT_TRUE(replay.GetPlaybackState() == PlaybackState::Stopped);
}

TEST(ReplaySystem_Camera_SetGetRoundTrip)
{
    auto& replay = ReplaySystem::GetInstance();

    replay.SetCamera(PlaybackCamera::FollowCam);
    EXPECT_TRUE(replay.GetCamera() == PlaybackCamera::FollowCam);

    replay.SetCamera(PlaybackCamera::FirstPerson);
    EXPECT_TRUE(replay.GetCamera() == PlaybackCamera::FirstPerson);
}

TEST(ReplaySystem_KillCamAndCamera_ConcurrentAccess_NoRace)
{
    auto& replay = ReplaySystem::GetInstance();

    std::atomic<bool> stop{false};

    std::thread writer(
        [&]
        {
            while (!stop.load())
            {
                replay.SetCamera(PlaybackCamera::FreeCam);
                replay.SetFollowEntity(1);
            }
        });

    std::thread reader(
        [&]
        {
            while (!stop.load())
            {
                (void)replay.GetCamera();
                (void)replay.IsKillCamActive();
            }
        });

    std::thread player(
        [&]
        {
            while (!stop.load())
            {
                replay.UpdatePlayback(0.001f);
            }
        });

    // Drive the kill-cam transitions (the previously-unguarded writers) against the
    // concurrent readers/mutators above.
    for (int i = 0; i < 5000; ++i)
    {
        replay.StartKillCam(0.1f, static_cast<uint32_t>(i));
        replay.StopKillCam();
    }

    stop.store(true);
    writer.join();
    reader.join();
    player.join();

    // Reaching here without a TSan-reported race (and without deadlock on the
    // non-recursive mutex) is the assertion.
    EXPECT_FALSE(replay.IsKillCamActive());
}
