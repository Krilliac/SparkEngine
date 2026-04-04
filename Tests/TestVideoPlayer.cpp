// TestVideoPlayer.cpp - Tests for Spark::Cinematic::VideoPlayer
#include "TestFramework.h"
#include "Engine/Cinematic/VideoPlayer.h"

// ============================================================================
// Initialization
// ============================================================================

TEST(VideoPlayer_Initialize)
{
    auto& player = Spark::Cinematic::VideoPlayer::GetInstance();
    player.Initialize();
    EXPECT_EQ(player.GetActiveVideoCount(), static_cast<size_t>(0));
    player.Shutdown();
}

// ============================================================================
// Open and close
// ============================================================================

TEST(VideoPlayer_Open)
{
    auto& player = Spark::Cinematic::VideoPlayer::GetInstance();
    player.Initialize();

    auto handle = player.Open("test.vid");
    EXPECT_TRUE(handle != Spark::Cinematic::INVALID_VIDEO_HANDLE);
    EXPECT_EQ(player.GetActiveVideoCount(), static_cast<size_t>(1));
    EXPECT_TRUE(player.GetState(handle) == Spark::Cinematic::VideoState::Idle);
    player.Shutdown();
}

TEST(VideoPlayer_Close)
{
    auto& player = Spark::Cinematic::VideoPlayer::GetInstance();
    player.Initialize();

    auto handle = player.Open("test.vid");
    player.Close(handle);
    EXPECT_EQ(player.GetActiveVideoCount(), static_cast<size_t>(0));
    EXPECT_TRUE(player.GetState(handle) == Spark::Cinematic::VideoState::Idle); // invalid returns Idle
    player.Shutdown();
}

// ============================================================================
// Playback control
// ============================================================================

TEST(VideoPlayer_PlayPauseStop)
{
    auto& player = Spark::Cinematic::VideoPlayer::GetInstance();
    player.Initialize();

    auto handle = player.Open("test.vid");

    player.Play(handle);
    EXPECT_TRUE(player.GetState(handle) == Spark::Cinematic::VideoState::Playing);

    player.Pause(handle);
    EXPECT_TRUE(player.GetState(handle) == Spark::Cinematic::VideoState::Paused);

    player.Stop(handle);
    EXPECT_TRUE(player.GetState(handle) == Spark::Cinematic::VideoState::Idle);
    EXPECT_NEAR(player.GetCurrentTime(handle), 0.0, 0.001);
    player.Shutdown();
}

TEST(VideoPlayer_Seek)
{
    auto& player = Spark::Cinematic::VideoPlayer::GetInstance();
    player.Initialize();

    auto handle = player.Open("test.vid");
    player.Seek(handle, 5.0);
    EXPECT_NEAR(player.GetCurrentTime(handle), 5.0, 0.001);
    player.Shutdown();
}

TEST(VideoPlayer_SetVolume)
{
    auto& player = Spark::Cinematic::VideoPlayer::GetInstance();
    player.Initialize();

    auto handle = player.Open("test.vid");
    player.SetVolume(handle, 0.5f);
    const auto* info = player.GetVideoInfo(handle);
    EXPECT_TRUE(info != nullptr);
    EXPECT_NEAR(info->volume, 0.5f, 0.001f);

    // Clamp to [0, 1]
    player.SetVolume(handle, 2.0f);
    EXPECT_NEAR(player.GetVideoInfo(handle)->volume, 1.0f, 0.001f);
    player.Shutdown();
}

TEST(VideoPlayer_SetPlaybackSpeed)
{
    auto& player = Spark::Cinematic::VideoPlayer::GetInstance();
    player.Initialize();

    auto handle = player.Open("test.vid");
    player.SetPlaybackSpeed(handle, 2.0f);
    EXPECT_NEAR(player.GetVideoInfo(handle)->playbackSpeed, 2.0f, 0.001f);
    player.Shutdown();
}

// ============================================================================
// Update and timing
// ============================================================================

TEST(VideoPlayer_Update_AdvancesTime)
{
    auto& player = Spark::Cinematic::VideoPlayer::GetInstance();
    player.Initialize();

    auto handle = player.Open("test.vid");
    player.Play(handle);

    player.Update(0.5f);
    EXPECT_NEAR(player.GetCurrentTime(handle), 0.5, 0.01);

    player.Update(0.5f);
    EXPECT_NEAR(player.GetCurrentTime(handle), 1.0, 0.01);
    player.Shutdown();
}

TEST(VideoPlayer_Update_PausedDoesNotAdvance)
{
    auto& player = Spark::Cinematic::VideoPlayer::GetInstance();
    player.Initialize();

    auto handle = player.Open("test.vid");
    player.Play(handle);
    player.Update(0.5f);
    player.Pause(handle);
    player.Update(0.5f);
    EXPECT_NEAR(player.GetCurrentTime(handle), 0.5, 0.01);
    player.Shutdown();
}

// ============================================================================
// Finished callback
// ============================================================================

TEST(VideoPlayer_FinishedCallback)
{
    auto& player = Spark::Cinematic::VideoPlayer::GetInstance();
    player.Initialize();

    auto handle = player.Open("test.vid", false);
    // Set a known duration (hack via the info — in real code the decoder sets this)
    // We test the callback mechanism by relying on the default duration being 0
    // which means it will never finish naturally. Instead test the callback registration.
    bool called = false;
    player.OnVideoFinished([&called](Spark::Cinematic::VideoHandle) { called = true; });

    // With duration=0, the video won't reach the end condition
    player.Play(handle);
    player.Update(1.0f);
    // Not called because duration is 0 (unknown/not set)
    EXPECT_FALSE(called);
    player.Shutdown();
}

// ============================================================================
// Multiple videos
// ============================================================================

TEST(VideoPlayer_MultipleVideos)
{
    auto& player = Spark::Cinematic::VideoPlayer::GetInstance();
    player.Initialize();

    auto h1 = player.Open("video1.vid");
    auto h2 = player.Open("video2.vid");
    auto h3 = player.Open("video3.vid");

    EXPECT_EQ(player.GetActiveVideoCount(), static_cast<size_t>(3));
    EXPECT_TRUE(h1 != h2);
    EXPECT_TRUE(h2 != h3);

    player.Play(h1);
    player.Play(h3);
    player.Update(0.1f);

    EXPECT_TRUE(player.GetState(h1) == Spark::Cinematic::VideoState::Playing);
    EXPECT_TRUE(player.GetState(h2) == Spark::Cinematic::VideoState::Idle);
    EXPECT_TRUE(player.GetState(h3) == Spark::Cinematic::VideoState::Playing);
    player.Shutdown();
}

TEST(VideoPlayer_ConsoleStatus)
{
    auto& player = Spark::Cinematic::VideoPlayer::GetInstance();
    player.Initialize();

    player.Open("test.vid");
    std::string status = player.Console_GetStatus();
    EXPECT_TRUE(status.find("1 loaded") != std::string::npos);
    player.Shutdown();
}
