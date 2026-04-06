// TestMovieRenderPipeline.cpp - Tests for Spark::Rendering::MovieRenderPipeline
#include "TestFramework.h"
#include "Engine/Rendering/MovieRenderPipeline.h"

// ============================================================================
// Initialization
// ============================================================================

TEST(MovieRenderPipeline_Initialize)
{
    auto& pipeline = Spark::Rendering::MovieRenderPipeline::GetInstance();
    pipeline.Initialize();
    EXPECT_FALSE(pipeline.IsRendering());
    EXPECT_NEAR(0.0f, pipeline.GetProgress(), 0.001f);
    pipeline.Shutdown();
}

// ============================================================================
// Settings validation
// ============================================================================

TEST(MovieRenderPipeline_SettingsDefaults)
{
    Spark::Rendering::MovieRenderSettings settings;
    EXPECT_EQ(static_cast<uint32_t>(1920), settings.width);
    EXPECT_EQ(static_cast<uint32_t>(1080), settings.height);
    EXPECT_NEAR(30.0f, settings.frameRate, 0.001f);
    EXPECT_EQ(static_cast<int32_t>(301), settings.GetTotalFrames());
}

TEST(MovieRenderPipeline_SettingsFixedDeltaTime)
{
    Spark::Rendering::MovieRenderSettings settings;
    settings.frameRate = 60.0f;
    EXPECT_NEAR(1.0f / 60.0f, settings.GetFixedDeltaTime(), 0.0001f);
}

TEST(MovieRenderPipeline_ApplyPreset)
{
    Spark::Rendering::MovieRenderSettings settings;
    settings.qualityPreset = Spark::Rendering::RenderQuality::Cinematic;
    settings.ApplyPreset();
    EXPECT_EQ(static_cast<uint32_t>(32), settings.aaSamples);
    EXPECT_EQ(static_cast<uint32_t>(16), settings.motionBlurSubFrames);
}

// ============================================================================
// Render lifecycle
// ============================================================================

TEST(MovieRenderPipeline_StartRender)
{
    auto& pipeline = Spark::Rendering::MovieRenderPipeline::GetInstance();
    pipeline.Initialize();

    Spark::Rendering::MovieRenderSettings settings;
    settings.startFrame = 0;
    settings.endFrame = 9;
    settings.qualityPreset = Spark::Rendering::RenderQuality::Preview;
    bool started = pipeline.StartRender(settings);
    EXPECT_TRUE(started);
    EXPECT_TRUE(pipeline.IsRendering());

    pipeline.Shutdown();
}

TEST(MovieRenderPipeline_CancelRender)
{
    auto& pipeline = Spark::Rendering::MovieRenderPipeline::GetInstance();
    pipeline.Initialize();

    Spark::Rendering::MovieRenderSettings settings;
    settings.startFrame = 0;
    settings.endFrame = 99;
    settings.qualityPreset = Spark::Rendering::RenderQuality::Preview;
    pipeline.StartRender(settings);
    EXPECT_TRUE(pipeline.IsRendering());

    pipeline.CancelRender();
    EXPECT_FALSE(pipeline.IsRendering());

    const auto& job = pipeline.GetCurrentJob();
    EXPECT_TRUE(job.state == Spark::Rendering::RenderJobState::Cancelled);

    pipeline.Shutdown();
}

TEST(MovieRenderPipeline_CannotStartWhileRendering)
{
    auto& pipeline = Spark::Rendering::MovieRenderPipeline::GetInstance();
    pipeline.Initialize();

    Spark::Rendering::MovieRenderSettings settings;
    settings.startFrame = 0;
    settings.endFrame = 9;
    settings.qualityPreset = Spark::Rendering::RenderQuality::Preview;
    EXPECT_TRUE(pipeline.StartRender(settings));
    EXPECT_FALSE(pipeline.StartRender(settings));

    pipeline.Shutdown();
}

TEST(MovieRenderPipeline_InvalidFrameRange)
{
    auto& pipeline = Spark::Rendering::MovieRenderPipeline::GetInstance();
    pipeline.Initialize();

    Spark::Rendering::MovieRenderSettings settings;
    settings.startFrame = 100;
    settings.endFrame = 0;
    bool started = pipeline.StartRender(settings);
    EXPECT_FALSE(started);
    EXPECT_FALSE(pipeline.IsRendering());

    pipeline.Shutdown();
}

// ============================================================================
// Frame stepping
// ============================================================================

TEST(MovieRenderPipeline_FrameStepping)
{
    auto& pipeline = Spark::Rendering::MovieRenderPipeline::GetInstance();
    pipeline.Initialize();

    Spark::Rendering::MovieRenderSettings settings;
    settings.startFrame = 0;
    settings.endFrame = 1;
    settings.aaSamples = 1;
    settings.motionBlurSubFrames = 1;
    settings.qualityPreset = Spark::Rendering::RenderQuality::Custom;
    pipeline.StartRender(settings);

    // Step through both frames (1 sub-frame each)
    pipeline.Update(0.033f);
    pipeline.Update(0.033f);
    EXPECT_FALSE(pipeline.IsRendering());

    const auto& job = pipeline.GetCurrentJob();
    EXPECT_TRUE(job.state == Spark::Rendering::RenderJobState::Complete);
    EXPECT_EQ(static_cast<size_t>(2), job.outputFiles.size());

    pipeline.Shutdown();
}

// ============================================================================
// Deterministic time controller
// ============================================================================

TEST(MovieRenderPipeline_DeterministicTime)
{
    Spark::Rendering::DeterministicTimeController ctrl;
    ctrl.Begin(1.0f / 30.0f, 0);
    EXPECT_TRUE(ctrl.IsActive());
    EXPECT_TRUE(ctrl.IsWarmUpComplete());
    EXPECT_NEAR(0.0f, ctrl.GetCurrentTime(), 0.0001f);

    float dt = ctrl.Step();
    EXPECT_NEAR(1.0f / 30.0f, dt, 0.0001f);
    EXPECT_EQ(static_cast<uint32_t>(1), ctrl.GetFrameIndex());

    ctrl.End();
    EXPECT_FALSE(ctrl.IsActive());
}

TEST(MovieRenderPipeline_WarmUpFrames)
{
    Spark::Rendering::DeterministicTimeController ctrl;
    ctrl.Begin(1.0f / 30.0f, 3);
    EXPECT_FALSE(ctrl.IsWarmUpComplete());

    ctrl.Step();
    ctrl.Step();
    EXPECT_FALSE(ctrl.IsWarmUpComplete());

    ctrl.Step();
    EXPECT_TRUE(ctrl.IsWarmUpComplete());
    ctrl.End();
}
